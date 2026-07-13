# TensorSched

A multi-threaded job scheduling simulator built for the Operating Systems course at FAST-NUCES. It implements a 3-level MLFQ scheduler with aging, preemption, and semaphore-limited concurrency, and shows all of it running live in an SDL2 dashboard (Gantt chart, queue depths, resource usage).



## What it does

Simulates jobs competing for CPU time under a real scheduling policy instead of just printing logs. Jobs get submitted with a priority, CPU%, memory, and burst length, and the scheduler decides what runs when, using:

- a 3-level multilevel feedback queue (MLFQ) with quanta of 2/4/8 ticks
- priority ordering within each level (CRITICAL > HIGH > MEDIUM > LOW, FIFO on ties)
- aging, so a job stuck in a low level for too long gets bumped back up instead of starving
- preemption, so a higher-priority job can interrupt one that's already running
- a semaphore that caps how many jobs can actually run at once (default 4), so it behaves like a fixed number of cores instead of unlimited parallelism

All of this is rendered in real time with SDL2  Gantt chart, per-level queue bars, CPU/memory sparklines, a job table, and an event log  so you can actually watch the scheduling decisions happen instead of reading through a trace file afterward.

## Why MLFQ

A simple round-robin or fixed-priority scheduler doesn't handle a workload where job lengths and priorities vary a lot — long CPU-bound jobs starve short ones, or low-priority jobs never get a turn at all. MLFQ adapts: everything starts at the top level, and jobs that don't finish in their quantum get pushed down to a level with a longer quantum but lower priority. Aging then prevents jobs at the bottom from getting stuck there forever.

## Architecture

| File | Responsibility |
|---|---|
| `job.h` | Job struct — atomic state fields, MLFQ metadata, timing |
| `job_queue.h` | `SingleLevelQueue` (mutex-protected priority-ordered list) and `MLFQJobQueue` (wraps 3 levels + aging) |
| `resource_manager.h` | Tracks CPU/memory usage, gates concurrency with a semaphore, exposes worker slot status |
| `scheduler.h/.cpp` | Owns the queue + resource manager, runs the dispatcher and aging threads |
| `main.cpp` | SDL2 event loop and all GUI panels, ~60 FPS |

Four thread types run at once:
- **Dispatcher** — pulls jobs off the queue, spawns worker threads, checks for preemptions
- **Aging thread** — wakes once a second, promotes any job that's waited too long
- **Worker threads** (up to 4 concurrently) — actually "run" a job tick by tick, then requeue or finish it
- **SDL2 main thread** — draws the dashboard, reading from ring buffers without holding locks

## Synchronization

- 4 separate mutexes for the job array, active-worker slots, Gantt ring buffer, and log ring buffer, so unrelated parts of the system don't block each other
- a semaphore initialized to `MAX_CONCURRENT` (4) limits how many jobs can run simultaneously — `sem_wait`/`sem_post` in `ResourceManager`
- preemption uses `compare_exchange_strong` on the job's atomic status instead of a mutex, so only one thread can ever win the race to preempt a given job
- worker threads are detached (clean themselves up); the dispatcher and aging threads are joinable and get joined explicitly in `Scheduler::stop()`

## GUI

- live MLFQ queue-depth bars per level, refreshed every 350ms
- scrollable Gantt strip, one block per tick per worker, colored by MLFQ level
- rolling sparklines for CPU and memory (120 samples)
- a submission form plus 6 one-click presets (Critical Burst, High CPU, Memory Hog, Bulk Low, Mixed Batch, Stress Test) for quickly generating a workload
- a stats tab: average wait/turnaround, throughput, and counts of submitted/completed/failed/preempted/promoted jobs

## Tech stack

- C++17
- POSIX threads + POSIX semaphores (Linux/macOS)
- a Win32 shim (`pthread_win.h`, `semaphore_win.h`) so the same scheduler code builds on Windows without changes — `platform.h` picks the right headers at compile time
- SDL2 + SDL2_ttf for the window, rendering, and text
- GNU Make

## Building

```bash
make
./tensorsched
```

Requires SDL2 and SDL2_ttf dev packages installed (`sdl2-config` needs to be on your path). The Makefile links `-lpthread` and whatever `sdl2-config --libs` returns.

On Windows, the pthread/semaphore shims are used instead of the real POSIX headers  same source, no `#ifdef`s scattered through the scheduler itself.

## Some code, if you're curious

Preemption, using CAS instead of a lock:

```cpp
bool Scheduler::requestPreemption(Job* target, const char* reason) {
    JobStatus expected = JobStatus::RUNNING;
    if (target->status.compare_exchange_strong(expected, JobStatus::PREEMPTED)) {
        stats_.total_preemptions++;
        target->preemption_count++;
        addLog(/* preemption log entry */);
        return true;
    }
    return false; // someone else already preempted it
}
```

Resource acquisition — block on the semaphore, then check the actual budget, and give the slot back if the job doesn't fit:

```cpp
bool acquire(int cpu, int mem) {
    sem_wait(&slots_);
    pthread_mutex_lock(&mutex_);
    bool ok = (used_cpu_ + cpu <= TOTAL_CPU) && (used_memory_ + mem <= TOTAL_MEMORY);
    if (ok) { used_cpu_ += cpu; used_memory_ += mem; }
    pthread_mutex_unlock(&mutex_);
    if (!ok) sem_post(&slots_);
    return ok;
}
```

## Known limitations

- Worker threads are detached rather than pooled, which is simpler but means thread lifetime isn't really managed — a proper thread pool would be cleaner (noted in the report as future work).
- No timed/scripted workload file yet, so every run's job mix is whatever you submit manually or via presets — not great for repeatable benchmarking.
- Only models CPU and memory as resources; no GPU or I/O bandwidth.
- `AGING_THRESHOLD` and the per-level quanta are hardcoded, not tuned automatically based on observed wait times.
- No CSV/export of Gantt or stats data for offline analysis.

