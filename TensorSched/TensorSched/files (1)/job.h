#pragma once

#include <atomic>
#include <chrono>
#include <cstring>

enum class JobStatus {
    WAITING,
    READY,
    RUNNING,
    PREEMPTED,
    COMPLETED,
    FAILED
};

enum class JobPriority {
    LOW      = 1,
    MEDIUM   = 2,
    HIGH     = 3,
    CRITICAL = 4
};

static constexpr int MLFQ_LEVELS     = 3;
static constexpr int MLFQ_QUANTUM[3] = { 2, 4, 8 };
static constexpr int AGING_THRESHOLD = 15;
static constexpr int MAX_JOBS        = 256;

struct Job {
    int         id;
    char        name[64];
    JobPriority priority;

    int cpu_required;
    int memory_required;
    int burst_time;

    std::atomic<JobStatus> status;
    std::atomic<int>  elapsed_ticks{0};
    std::atomic<int>  wait_ticks{0};
    std::atomic<int>  preemption_count{0};
    std::atomic<int>  mlfq_level{0};
    std::atomic<int>  ticks_this_slice{0};
    std::atomic<int>  aging_ticks{0};
    std::atomic<bool> has_started{false};
    std::atomic<int>  assigned_worker{-1};

    std::chrono::steady_clock::time_point submit_time;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point finish_time;
    std::chrono::steady_clock::time_point last_ready_time;

    Job(int id_, const char* name_, JobPriority prio,
        int cpu, int mem, int burst)
        : id(id_), priority(prio),
          cpu_required(cpu), memory_required(mem),
          burst_time(burst),
          status(JobStatus::WAITING)
    {
        std::strncpy(name, name_, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        submit_time     = std::chrono::steady_clock::now();
        last_ready_time = submit_time;
    }

    Job(const Job&)            = delete;
    Job& operator=(const Job&) = delete;

    int priorityValue() const { return static_cast<int>(priority); }

    double completionPct() const {
        if (burst_time <= 0) return 100.0;
        double p = static_cast<double>(elapsed_ticks.load()) /
                   static_cast<double>(burst_time) * 100.0;
        return (p > 100.0) ? 100.0 : p;
    }

    int remainingTicks() const {
        int rem = burst_time - elapsed_ticks.load();
        return (rem < 0) ? 0 : rem;
    }

    int ticksLeftInSlice() const {
        int lvl = mlfq_level.load();
        if (lvl < 0 || lvl >= MLFQ_LEVELS) lvl = MLFQ_LEVELS - 1;
        int left = MLFQ_QUANTUM[lvl] - ticks_this_slice.load();
        return (left < 0) ? 0 : left;
    }

    bool quantumExpired() const {
        int lvl = mlfq_level.load();
        if (lvl < 0 || lvl >= MLFQ_LEVELS) lvl = MLFQ_LEVELS - 1;
        return ticks_this_slice.load() >= MLFQ_QUANTUM[lvl];
    }

    static const char* statusStr(JobStatus s) {
        switch (s) {
            case JobStatus::WAITING:   return "WAITING";
            case JobStatus::READY:     return "READY";
            case JobStatus::RUNNING:   return "RUNNING";
            case JobStatus::PREEMPTED: return "PREEMPTED";
            case JobStatus::COMPLETED: return "COMPLETED";
            case JobStatus::FAILED:    return "FAILED";
        }
        return "UNKNOWN";
    }

    static const char* priorityStr(JobPriority p) {
        switch (p) {
            case JobPriority::LOW:      return "LOW";
            case JobPriority::MEDIUM:   return "MEDIUM";
            case JobPriority::HIGH:     return "HIGH";
            case JobPriority::CRITICAL: return "CRITICAL";
        }
        return "UNKNOWN";
    }
};