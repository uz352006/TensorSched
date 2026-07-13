#pragma once
#include "platform.h"
#include "job.h"
#include "job_queue.h"
#include "resource_manager.h"
#include <atomic>
#include <functional>
#include <cstring>
struct SchedulerStats {
    std::atomic<int>    total_submitted{0};
    std::atomic<int>    total_completed{0};
    std::atomic<int>    total_failed{0};
    std::atomic<int>    total_preemptions{0};
    std::atomic<int>    total_aging_promotions{0};
    std::atomic<double> avg_wait_ticks{0.0};
    std::atomic<double> avg_turnaround_sec{0.0};
    std::atomic<double> throughput{0.0};
    std::atomic<double> cpu_utilization{0.0};
};
struct GanttRecord {
    int  job_id;
    int  worker;
    int  mlfq_level;
    int  priority;
    int  tick;        
    bool preempted;
    char job_name[32];
};

static constexpr int MAX_GANTT = 2000;
static constexpr int MAX_LOG = 500;
struct LogEntry {
    char    text[256];
    int     color_hint;  // 0=normal 1=running 2=done 3=failed 4=preempt 5=system
};
class Scheduler {
public:
    using LogCallback = std::function<void(const char*, int /*color_hint*/)>;
    using DoneCallback = std::function<void(Job*)>;

    explicit Scheduler(LogCallback logCb  = nullptr,DoneCallback doneCb = nullptr);
    ~Scheduler();
    void submitJob(Job* job);

    void start();
    void stop();
    bool running() const { return running_.load(); }
    ResourceManager& resources()  { return resources_; }
    SchedulerStats&  stats()      { return stats_; }
    MLFQJobQueue&    queue()      { return queue_; }
    int  allJobsSnapshot(Job** buf, int max_count) const;
    int  logSnapshot(LogEntry* buf, int max_count) const;
    int  ganttSnapshot(GanttRecord* buf, int max_count) const;
    int  globalTick()  const { return global_tick_.load(); }

    bool requestPreemption(Job* target, const char* reason);

private:
    static void* dispatchLoop(void* arg);
    void         runDispatcher();
    struct WorkerArg {
        Scheduler* sched;
        Job*       job;
        int        worker_slot;
    };
    static void* workerFunc(void* arg);
    static void* agingLoop(void* arg);
    void         runAging();

    void checkPreemptions();

    void addLog(const char* msg, int color_hint = 0);
    void addGantt(int job_id, int worker, int mlfq_level,
                  int priority, bool preempted, const char* name);
    void recalcStats();

    MLFQJobQueue    queue_;
    ResourceManager resources_;
    SchedulerStats  stats_;

    Job*                    all_jobs_[MAX_JOBS];
    int                     all_jobs_count_;
    mutable pthread_mutex_t jobs_mutex_;

    Job*                    active_jobs_[MAX_CONCURRENT];
    mutable pthread_mutex_t active_mutex_;

    GanttRecord             gantt_buf_[MAX_GANTT];
    int                     gantt_head_;
    int                     gantt_count_;
    mutable pthread_mutex_t gantt_mutex_;

    LogEntry                log_buf_[MAX_LOG];
    int                     log_head_;
    int                     log_count_;
    mutable pthread_mutex_t log_mutex_;

    std::atomic<int>  global_tick_{0};
    pthread_t         dispatcher_thread_;
    pthread_t         aging_thread_;
    std::atomic<bool> running_{false};

    LogCallback  log_cb_;
    DoneCallback done_cb_;
};
