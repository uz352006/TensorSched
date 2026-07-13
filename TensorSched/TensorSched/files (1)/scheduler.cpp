#include "scheduler.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>

static void timestamp(char* buf, int bufsz) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm   = std::localtime(&t);
    std::strftime(buf, (size_t)bufsz, "%H:%M:%S", tm);
}

static const char* mlfqLevelStr(int lvl) {
    switch (lvl) {
        case 0: return "L0[HIGH]";
        case 1: return "L1[MID ]";
        case 2: return "L2[LOW ]";
    }
    return "L?";
}

Scheduler::Scheduler(LogCallback logCb, DoneCallback doneCb)
    : all_jobs_count_(0),
      gantt_head_(0), gantt_count_(0),
      log_head_(0),   log_count_(0),
      log_cb_(logCb), done_cb_(doneCb)
{
    pthread_mutex_init(&jobs_mutex_,   nullptr);
    pthread_mutex_init(&active_mutex_, nullptr);
    pthread_mutex_init(&gantt_mutex_,  nullptr);
    pthread_mutex_init(&log_mutex_,    nullptr);

    std::memset(all_jobs_,    0, sizeof(all_jobs_));
    std::memset(active_jobs_, 0, sizeof(active_jobs_));
    std::memset(gantt_buf_,   0, sizeof(gantt_buf_));
    std::memset(log_buf_,     0, sizeof(log_buf_));
}

Scheduler::~Scheduler() {
    stop();

    pthread_mutex_lock(&jobs_mutex_);
    for (int i = 0; i < all_jobs_count_; i++) {
        delete all_jobs_[i];
        all_jobs_[i] = nullptr;
    }
    all_jobs_count_ = 0;
    pthread_mutex_unlock(&jobs_mutex_);

    pthread_mutex_destroy(&jobs_mutex_);
    pthread_mutex_destroy(&active_mutex_);
    pthread_mutex_destroy(&gantt_mutex_);
    pthread_mutex_destroy(&log_mutex_);
}

void Scheduler::start() {
    if (running_.load()) return;
    running_ = true;

    pthread_create(&dispatcher_thread_, nullptr, dispatchLoop, this);
    pthread_create(&aging_thread_,      nullptr, agingLoop,    this);

    addLog("[SCHEDULER] ==========================================", 5);
    addLog("[SCHEDULER] TensorSched v3.0 — MLFQ Scheduler online", 5);

    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "[SCHEDULER] Levels: L0=%dt  L1=%dt  L2=%dt  (quantum/level)",
        MLFQ_QUANTUM[0], MLFQ_QUANTUM[1], MLFQ_QUANTUM[2]);
    addLog(msg, 5);

    std::snprintf(msg, sizeof(msg),
        "[SCHEDULER] Aging threshold: %d wait-ticks → promotion",
        AGING_THRESHOLD);
    addLog(msg, 5);

    std::snprintf(msg, sizeof(msg),
        "[SCHEDULER] Max concurrency: %d parallel workers  |  Preemption: ON  |  RR: ON",
        MAX_CONCURRENT);
    addLog(msg, 5);
    addLog("[SCHEDULER] ==========================================", 5);
}

void Scheduler::stop() {
    if (!running_.load()) return;
    running_ = false;
    pthread_join(dispatcher_thread_, nullptr);
    pthread_join(aging_thread_,      nullptr);
    addLog("[SCHEDULER] All threads stopped cleanly.", 5);
}

void Scheduler::submitJob(Job* job) {
    if (!job) return;

    pthread_mutex_lock(&jobs_mutex_);
    if (all_jobs_count_ < MAX_JOBS) {
        all_jobs_[all_jobs_count_++] = job;
    }
    pthread_mutex_unlock(&jobs_mutex_);

    stats_.total_submitted++;

    char ts[16], msg[256];
    timestamp(ts, sizeof(ts));
    std::snprintf(msg, sizeof(msg),
        "[%s] [SUBMIT] Job #%d \"%s\" | Prio:%s | CPU:%d%% | MEM:%dMB | Burst:%dt → %s",
        ts, job->id, job->name,
        Job::priorityStr(job->priority),
        job->cpu_required, job->memory_required, job->burst_time,
        mlfqLevelStr(0));
    addLog(msg, 0);

    queue_.pushNew(job);
}

void* Scheduler::dispatchLoop(void* arg) {
    static_cast<Scheduler*>(arg)->runDispatcher();
    return nullptr;
}

void Scheduler::runDispatcher() {
    int cycle = 0;

    while (running_.load()) {
        if (++cycle % 4 == 0) {
            queue_.tickWaiting();
            global_tick_++;
        }

        checkPreemptions();

        Job* job = queue_.tryPop();
        if (!job) {
            usleep(50000);
            continue;
        }

        int slot = resources_.getFreeWorkerSlot();

        WorkerArg* warg = new WorkerArg{this, job, slot};
        pthread_t  tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, workerFunc, warg);
        pthread_attr_destroy(&attr);
    }
}

void Scheduler::checkPreemptions() {
    pthread_mutex_lock(&active_mutex_);
    for (int slot = 0; slot < MAX_CONCURRENT; slot++) {
        Job* j = active_jobs_[slot];
        if (!j) continue;
        if (j->status.load() != JobStatus::RUNNING) continue;

        int jlvl  = j->mlfq_level.load();
        int jprio = j->priorityValue();

        if (queue_.hasBetterCandidate(jlvl, jprio)) {
            requestPreemption(j, "Higher-priority job arrived");
        }
    }
    pthread_mutex_unlock(&active_mutex_);
}

bool Scheduler::requestPreemption(Job* target, const char* reason) {
    if (!target) return false;

    JobStatus expected = JobStatus::RUNNING;
    if (target->status.compare_exchange_strong(expected, JobStatus::PREEMPTED)) {
        stats_.total_preemptions++;
        target->preemption_count++;

        char ts[16], msg[256];
        timestamp(ts, sizeof(ts));
        std::snprintf(msg, sizeof(msg),
            "[%s] [PREEMPT] Job #%d \"%s\" at %s | Reason: %s | Count: %d",
            ts, target->id, target->name,
            mlfqLevelStr(target->mlfq_level.load()),
            reason, target->preemption_count.load());
        addLog(msg, 4);
        return true;
    }
    return false;
}

void* Scheduler::workerFunc(void* arg) {
    WorkerArg* warg  = static_cast<WorkerArg*>(arg);
    Scheduler* sched = warg->sched;
    Job*       job   = warg->job;
    int        slot  = warg->worker_slot;
    delete warg;

    char ts[16], msg[256];

    timestamp(ts, sizeof(ts));
    std::snprintf(msg, sizeof(msg),
        "[%s] [QUEUING] Job #%d \"%s\" | %s | Prio:%s | Waiting for worker slot...",
        ts, job->id, job->name,
        mlfqLevelStr(job->mlfq_level.load()),
        Job::priorityStr(job->priority));
    sched->addLog(msg, 0);

    bool ok = sched->resources_.acquire(job->cpu_required, job->memory_required);

    if (!ok) {
        job->status = JobStatus::FAILED;
        sched->stats_.total_failed++;
        timestamp(ts, sizeof(ts));
        std::snprintf(msg, sizeof(msg),
            "[%s] [FAILED] Job #%d \"%s\" — insufficient resources "
            "(need CPU:%d%% MEM:%dMB, available CPU:%d%% MEM:%dMB)",
            ts, job->id, job->name,
            job->cpu_required, job->memory_required,
            sched->resources_.freeCPU(), sched->resources_.freeMemory());
        sched->addLog(msg, 3);
        if (sched->done_cb_) sched->done_cb_(job);
        return nullptr;
    }

    if (slot < 0) {
        slot = sched->resources_.getFreeWorkerSlot();
        if (slot < 0) slot = 0;
    }

    pthread_mutex_lock(&sched->active_mutex_);
    for (int i = 0; i < MAX_CONCURRENT; i++) {
        if (!sched->active_jobs_[(slot + i) % MAX_CONCURRENT]) {
            slot = (slot + i) % MAX_CONCURRENT;
            break;
        }
    }
    sched->active_jobs_[slot] = job;
    pthread_mutex_unlock(&sched->active_mutex_);

    job->status          = JobStatus::RUNNING;
    job->assigned_worker = slot;

    if (!job->has_started.load()) {
        job->has_started = true;
        job->start_time  = std::chrono::steady_clock::now();
    }

    sched->resources_.setWorkerBusy(slot, job->id,
                                    job->priorityValue(),
                                    job->mlfq_level.load(),
                                    job->name);

    timestamp(ts, sizeof(ts));
    std::snprintf(msg, sizeof(msg),
        "[%s] [RUNNING] Job #%d \"%s\" | Worker-%d | %s | Prio:%s | "
        "Quantum:%dt | Remaining:%dt",
        ts, job->id, job->name, slot,
        mlfqLevelStr(job->mlfq_level.load()),
        Job::priorityStr(job->priority),
        MLFQ_QUANTUM[job->mlfq_level.load()],
        job->remainingTicks());
    sched->addLog(msg, 1);

    bool was_preempted = false;

    while (job->elapsed_ticks.load() < job->burst_time && sched->running_.load()) {

        if (job->status.load() == JobStatus::PREEMPTED) {
            was_preempted = true;
            break;
        }

        if (job->quantumExpired()) {
            job->status   = JobStatus::PREEMPTED;
            was_preempted = true;
            timestamp(ts, sizeof(ts));
            std::snprintf(msg, sizeof(msg),
                "[%s] [QUANTUM] Job #%d \"%s\" exhausted quantum at %s "
                "(Q=%dt used)",
                ts, job->id, job->name,
                mlfqLevelStr(job->mlfq_level.load()),
                MLFQ_QUANTUM[job->mlfq_level.load()]);
            sched->addLog(msg, 0);
            break;
        }

        usleep(200000);
        job->elapsed_ticks++;
        job->ticks_this_slice++;

        sched->addGantt(job->id, slot, job->mlfq_level.load(),
                        job->priorityValue(), false, job->name);
    }

    pthread_mutex_lock(&sched->active_mutex_);
    sched->active_jobs_[slot] = nullptr;
    pthread_mutex_unlock(&sched->active_mutex_);

    sched->resources_.setWorkerFree(slot);
    sched->resources_.release(job->cpu_required, job->memory_required);

    if (job->elapsed_ticks.load() >= job->burst_time) {
        job->finish_time = std::chrono::steady_clock::now();
        job->status      = JobStatus::COMPLETED;
        sched->stats_.total_completed++;
        sched->recalcStats();

        double turnaround = std::chrono::duration<double>(
            job->finish_time - job->submit_time).count();

        timestamp(ts, sizeof(ts));
        std::snprintf(msg, sizeof(msg),
            "[%s] [DONE] Job #%d \"%s\" | Turnaround:%.1fs | "
            "Preemptions:%d | Final:%s",
            ts, job->id, job->name,
            turnaround,
            job->preemption_count.load(),
            mlfqLevelStr(job->mlfq_level.load()));
        sched->addLog(msg, 2);

        if (sched->done_cb_) sched->done_cb_(job);

    } else if (was_preempted) {
        bool demote = job->quantumExpired();

        int old_level = job->mlfq_level.load();
        sched->queue_.requeuePreempted(job, demote);
        int new_level = job->mlfq_level.load();

        timestamp(ts, sizeof(ts));
        std::snprintf(msg, sizeof(msg),
            "[%s] [REQUEUE] Job #%d \"%s\" | %s → %s | "
            "Done:%.0f%% | Remain:%dt",
            ts, job->id, job->name,
            mlfqLevelStr(old_level), mlfqLevelStr(new_level),
            job->completionPct(), job->remainingTicks());
        sched->addLog(msg, 0);
    }

    return nullptr;
}

void* Scheduler::agingLoop(void* arg) {
    static_cast<Scheduler*>(arg)->runAging();
    return nullptr;
}

void Scheduler::runAging() {
    while (running_.load()) {
        usleep(1000000);
        if (!running_.load()) break;

        int promoted = queue_.runAging();
        if (promoted > 0) {
            stats_.total_aging_promotions += promoted;
            char ts[16], msg[256];
            timestamp(ts, sizeof(ts));
            std::snprintf(msg, sizeof(msg),
                "[%s] [AGING] %d job(s) promoted (starvation prevention) | "
                "Total promotions: %d",
                ts, promoted, stats_.total_aging_promotions.load());
            addLog(msg, 5);
        }
    }
}

void Scheduler::addGantt(int job_id, int worker, int mlfq_level,
                          int priority, bool preempted, const char* name) {
    pthread_mutex_lock(&gantt_mutex_);
    GanttRecord& g = gantt_buf_[gantt_head_ % MAX_GANTT];
    g.job_id     = job_id;
    g.worker     = worker;
    g.mlfq_level = mlfq_level;
    g.priority   = priority;
    g.tick       = global_tick_.load();
    g.preempted  = preempted;
    std::strncpy(g.job_name, name, 31);
    g.job_name[31] = '\0';
    gantt_head_++;
    if (gantt_count_ < MAX_GANTT) gantt_count_++;
    pthread_mutex_unlock(&gantt_mutex_);
}

int Scheduler::ganttSnapshot(GanttRecord* buf, int max_count) const {
    pthread_mutex_lock(&gantt_mutex_);
    int count = (gantt_count_ < max_count) ? gantt_count_ : max_count;
    int start = (gantt_head_ - count + MAX_GANTT) % MAX_GANTT;
    for (int i = 0; i < count; i++) {
        buf[i] = gantt_buf_[(start + i) % MAX_GANTT];
    }
    pthread_mutex_unlock(&gantt_mutex_);
    return count;
}

void Scheduler::addLog(const char* text, int color_hint) {
    pthread_mutex_lock(&log_mutex_);
    LogEntry& e = log_buf_[log_head_ % MAX_LOG];
    std::strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';
    e.color_hint = color_hint;
    log_head_++;
    if (log_count_ < MAX_LOG) log_count_++;
    pthread_mutex_unlock(&log_mutex_);

    if (log_cb_) log_cb_(text, color_hint);
}

int Scheduler::logSnapshot(LogEntry* buf, int max_count) const {
    pthread_mutex_lock(&log_mutex_);
    int count = (log_count_ < max_count) ? log_count_ : max_count;
    int start = (log_head_ - count + MAX_LOG) % MAX_LOG;
    for (int i = 0; i < count; i++) {
        buf[i] = log_buf_[(start + i) % MAX_LOG];
    }
    pthread_mutex_unlock(&log_mutex_);
    return count;
}

int Scheduler::allJobsSnapshot(Job** buf, int max_count) const {
    pthread_mutex_lock(&jobs_mutex_);
    int count = (all_jobs_count_ < max_count) ? all_jobs_count_ : max_count;
    for (int i = 0; i < count; i++) buf[i] = all_jobs_[i];
    pthread_mutex_unlock(&jobs_mutex_);
    return count;
}

void Scheduler::recalcStats() {
    Job* jobs[MAX_JOBS];
    int  count = allJobsSnapshot(jobs, MAX_JOBS);

    double total_wait = 0.0;
    double total_turn = 0.0;
    int    completed  = 0;

    for (int i = 0; i < count; i++) {
        Job* j = jobs[i];
        if (j->status.load() == JobStatus::COMPLETED) {
            total_wait += j->wait_ticks.load();
            double turn = std::chrono::duration<double>(
                j->finish_time - j->submit_time).count();
            total_turn += turn;
            completed++;
        }
    }

    if (completed > 0) {
        stats_.avg_wait_ticks     = total_wait / completed;
        stats_.avg_turnaround_sec = total_turn  / completed;
        double avg_turn = total_turn / completed;
        stats_.throughput = (avg_turn > 0.0) ?
            static_cast<double>(completed) / avg_turn : 0.0;
    }

    stats_.cpu_utilization = resources_.cpuPct();
}