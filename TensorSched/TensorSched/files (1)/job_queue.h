#pragma once
#include "platform.h"
#include "job.h"
#include <cstring>

struct QueueNode {
    Job*       job;
    QueueNode* next;

    explicit QueueNode(Job* j) : job(j), next(nullptr) {}
};

class SingleLevelQueue {
public:
    SingleLevelQueue() : head_(nullptr), size_(0) {
        pthread_mutex_init(&mutex_, nullptr);
    }

    ~SingleLevelQueue() {
        clear();
        pthread_mutex_destroy(&mutex_);
    }

    void push(Job* job) {
        pthread_mutex_lock(&mutex_);
        QueueNode* node = new QueueNode(job);

        if (!head_) {
            head_ = node;
        } else {
            QueueNode* prev = nullptr;
            QueueNode* cur  = head_;

            while (cur) {
                bool insert_here = false;
                if (job->priorityValue() > cur->job->priorityValue()) {
                    insert_here = true;
                } else if (job->priorityValue() == cur->job->priorityValue()) {
                    insert_here = (job->submit_time < cur->job->submit_time);
                }

                if (insert_here) break;
                prev = cur;
                cur  = cur->next;
            }

            if (!prev) {
                node->next = head_;
                head_      = node;
            } else {
                node->next = prev->next;
                prev->next = node;
            }
        }

        ++size_;
        pthread_mutex_unlock(&mutex_);
    }

    Job* tryPop() {
        pthread_mutex_lock(&mutex_);
        if (!head_) {
            pthread_mutex_unlock(&mutex_);
            return nullptr;
        }
        QueueNode* node = head_;
        head_           = head_->next;
        --size_;
        Job* job = node->job;
        delete node;
        pthread_mutex_unlock(&mutex_);
        return job;
    }

    int topPriority() const {
        pthread_mutex_lock(&mutex_);
        int p = head_ ? head_->job->priorityValue() : -1;
        pthread_mutex_unlock(&mutex_);
        return p;
    }

    bool empty() const {
        pthread_mutex_lock(&mutex_);
        bool e = (head_ == nullptr);
        pthread_mutex_unlock(&mutex_);
        return e;
    }

    int size() const {
        pthread_mutex_lock(&mutex_);
        int s = size_;
        pthread_mutex_unlock(&mutex_);
        return s;
    }

    void tickWaiting() {
        pthread_mutex_lock(&mutex_);
        QueueNode* cur = head_;
        while (cur) {
            cur->job->wait_ticks++;
            cur->job->aging_ticks++;
            cur = cur->next;
        }
        pthread_mutex_unlock(&mutex_);
    }

    int drainAged(Job** out, int out_size) {
        pthread_mutex_lock(&mutex_);
        int count = 0;

        QueueNode* prev = nullptr;
        QueueNode* cur  = head_;

        while (cur && count < out_size) {
            if (cur->job->aging_ticks.load() >= AGING_THRESHOLD) {
                QueueNode* aged = cur;
                if (prev) {
                    prev->next = cur->next;
                } else {
                    head_ = cur->next;
                }
                cur = cur->next;
                out[count++] = aged->job;
                delete aged;
                --size_;
            } else {
                prev = cur;
                cur  = cur->next;
            }
        }

        pthread_mutex_unlock(&mutex_);
        return count;
    }

    int snapshot(Job** buf, int max_count) const {
        pthread_mutex_lock(&mutex_);
        int count = 0;
        QueueNode* cur = head_;
        while (cur && count < max_count) {
            buf[count++] = cur->job;
            cur = cur->next;
        }
        pthread_mutex_unlock(&mutex_);
        return count;
    }

private:
    void clear() {
        pthread_mutex_lock(&mutex_);
        QueueNode* cur = head_;
        while (cur) {
            QueueNode* next = cur->next;
            delete cur;
            cur = next;
        }
        head_  = nullptr;
        size_  = 0;
        pthread_mutex_unlock(&mutex_);
    }

    mutable pthread_mutex_t mutex_;
    QueueNode* head_;
    int        size_;
};

class MLFQJobQueue {
public:
    MLFQJobQueue() {
        pthread_mutex_init(&global_mutex_, nullptr);
    }
    ~MLFQJobQueue() {
        pthread_mutex_destroy(&global_mutex_);
    }

    void pushNew(Job* job) {
        job->mlfq_level       = 0;
        job->ticks_this_slice = 0;
        job->aging_ticks      = 0;
        job->status           = JobStatus::READY;
        job->last_ready_time  = std::chrono::steady_clock::now();
        levels_[0].push(job);
    }

    void requeuePreempted(Job* job, bool demote) {
        if (demote) {
            int newLevel = job->mlfq_level.load() + 1;
            if (newLevel >= MLFQ_LEVELS) newLevel = MLFQ_LEVELS - 1;
            job->mlfq_level = newLevel;
        }
        job->ticks_this_slice = 0;
        job->aging_ticks      = 0;
        job->status           = JobStatus::READY;
        job->last_ready_time  = std::chrono::steady_clock::now();
        levels_[job->mlfq_level.load()].push(job);
    }

    Job* tryPop() {
        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            Job* job = levels_[lvl].tryPop();
            if (job) return job;
        }
        return nullptr;
    }

    bool hasBetterCandidate(int running_level, int running_prio) const {
        for (int lvl = 0; lvl < running_level; lvl++) {
            if (!levels_[lvl].empty()) return true;
        }
        if (running_level < MLFQ_LEVELS && !levels_[running_level].empty()) {
            if (levels_[running_level].topPriority() > running_prio)
                return true;
        }
        return false;
    }

    bool empty() const {
        for (int l = 0; l < MLFQ_LEVELS; l++)
            if (!levels_[l].empty()) return false;
        return true;
    }

    int size() const {
        int total = 0;
        for (int l = 0; l < MLFQ_LEVELS; l++) total += levels_[l].size();
        return total;
    }

    int sizeAtLevel(int l) const {
        if (l < 0 || l >= MLFQ_LEVELS) return 0;
        return levels_[l].size();
    }

    int runAging() {
        static constexpr int AGED_BUF = 64;
        Job* aged[AGED_BUF];
        int  promoted = 0;

        for (int lvl = 1; lvl < MLFQ_LEVELS; lvl++) {
            int count = levels_[lvl].drainAged(aged, AGED_BUF);
            for (int i = 0; i < count; i++) {
                Job* job          = aged[i];
                int  newLvl       = lvl - 1;
                job->mlfq_level       = newLvl;
                job->ticks_this_slice = 0;
                job->aging_ticks      = 0;
                job->last_ready_time  = std::chrono::steady_clock::now();
                levels_[newLvl].push(job);
                promoted++;
            }
        }
        return promoted;
    }

    void tickWaiting() {
        for (int l = 0; l < MLFQ_LEVELS; l++)
            levels_[l].tickWaiting();
    }

    int snapshot(Job** buf, int max_count) const {
        int total = 0;
        for (int l = 0; l < MLFQ_LEVELS; l++) {
            int got = levels_[l].snapshot(buf + total, max_count - total);
            total += got;
            if (total >= max_count) break;
        }
        return total;
    }

private:
    SingleLevelQueue        levels_[MLFQ_LEVELS];
    mutable pthread_mutex_t global_mutex_;
};

using JobQueue = MLFQJobQueue;