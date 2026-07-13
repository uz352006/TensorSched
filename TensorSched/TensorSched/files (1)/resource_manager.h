#pragma once
#include "platform.h"
#include <atomic>
#include <cstring>
#include <cstdio>
static constexpr int TOTAL_CPU      = 100;  
static constexpr int TOTAL_MEMORY   = 4096; 
static constexpr int MAX_CONCURRENT = 4;    
struct WorkerSlot {
    std::atomic<bool> busy{false};
    std::atomic<int>  job_id{-1};
    std::atomic<int>  priority{0};
    std::atomic<int>  mlfq_level{0};
    char              job_name[64];

    WorkerSlot() { std::memset(job_name, 0, sizeof(job_name)); }
};
class ResourceManager {
public:
    ResourceManager() : used_cpu_(0), used_memory_(0), active_jobs_(0) {
        pthread_mutex_init(&mutex_, nullptr);
        sem_init(&slots_, 0, MAX_CONCURRENT);
    }

    ~ResourceManager() {
        pthread_mutex_destroy(&mutex_);
        sem_destroy(&slots_);
    }
    bool acquire(int cpu, int mem) {
        sem_wait(&slots_);  // blocks if all slots in use
        pthread_mutex_lock(&mutex_);
        bool ok = (used_cpu_.load()    + cpu <= TOTAL_CPU) &&
                  (used_memory_.load() + mem <= TOTAL_MEMORY);
        if (ok) {
            used_cpu_    += cpu;
            used_memory_ += mem;
            active_jobs_++;
        }
        pthread_mutex_unlock(&mutex_);
        if (!ok) sem_post(&slots_);  
        return ok;
    }
    void release(int cpu, int mem) {
        pthread_mutex_lock(&mutex_);
        used_cpu_    -= cpu;
        used_memory_ -= mem;
        if (used_cpu_    < 0) used_cpu_    = 0;
        if (used_memory_ < 0) used_memory_ = 0;
        active_jobs_--;
        if (active_jobs_ < 0) active_jobs_ = 0;
        pthread_mutex_unlock(&mutex_);
        sem_post(&slots_);  // wake one waiting job
    }
    void setWorkerBusy(int slot, int job_id, int priority,
                       int mlfq_level, const char* name) {
        if (slot < 0 || slot >= MAX_CONCURRENT) return;
        pthread_mutex_lock(&mutex_);
        workers_[slot].busy       = true;
        workers_[slot].job_id     = job_id;
        workers_[slot].priority   = priority;
        workers_[slot].mlfq_level = mlfq_level;
        std::snprintf(workers_[slot].job_name,
                      sizeof(workers_[slot].job_name), "%s", name);
        pthread_mutex_unlock(&mutex_);
    }
    void setWorkerFree(int slot) {
        if (slot < 0 || slot >= MAX_CONCURRENT) return;
        pthread_mutex_lock(&mutex_);
        workers_[slot].busy           = false;
        workers_[slot].job_id         = -1;
        workers_[slot].job_name[0]    = '\0';
        workers_[slot].priority       = 0;
        workers_[slot].mlfq_level     = 0;
        pthread_mutex_unlock(&mutex_);
    }
    int getFreeWorkerSlot() const {
        pthread_mutex_lock(&mutex_);
        int found = -1;
        for (int i = 0; i < MAX_CONCURRENT; i++) {
            if (!workers_[i].busy.load()) { found = i; break; }
        }
        pthread_mutex_unlock(&mutex_);
        return found;
    }
    const WorkerSlot& getWorker(int i) const {
        return workers_[i < 0 ? 0 : (i >= MAX_CONCURRENT ? MAX_CONCURRENT - 1 : i)];
    }
    int    usedCPU()    const { return used_cpu_.load(); }
    int    usedMemory() const { return used_memory_.load(); }
    int    activeJobs() const { return active_jobs_.load(); }
    int    freeCPU()    const { return TOTAL_CPU    - used_cpu_.load(); }
    int    freeMemory() const { return TOTAL_MEMORY - used_memory_.load(); }
    double cpuPct()     const { return used_cpu_.load()    * 100.0 / TOTAL_CPU; }
    double memoryPct()  const { return used_memory_.load() * 100.0 / TOTAL_MEMORY; }
private:
    std::atomic<int>        used_cpu_, used_memory_, active_jobs_;
    mutable pthread_mutex_t mutex_;
    sem_t                   slots_;
    WorkerSlot              workers_[MAX_CONCURRENT];
};
