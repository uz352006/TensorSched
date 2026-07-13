#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <cstdlib>

typedef HANDLE pthread_t;

typedef struct {
    int detachstate;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

inline int pthread_attr_init(pthread_attr_t* attr) {
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}
inline int pthread_attr_destroy(pthread_attr_t* /*attr*/) { return 0; }
inline int pthread_attr_setdetachstate(pthread_attr_t* attr, int state) {
    attr->detachstate = state;
    return 0;
}

struct _WinThreadArg {
    void* (*func)(void*);
    void* arg;
};

inline unsigned __stdcall _winThreadEntry(void* p) {
    auto* a = static_cast<_WinThreadArg*>(p);
    a->func(a->arg);
    delete a;
    return 0;
}

inline int pthread_create(pthread_t* tid, const pthread_attr_t* attr,
                           void* (*func)(void*), void* arg) {
    auto* a = new _WinThreadArg{func, arg};
    bool detach = attr && attr->detachstate == PTHREAD_CREATE_DETACHED;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, _winThreadEntry, a, 0, nullptr);
    if (!h) { delete a; return -1; }
    if (detach) { CloseHandle(h); *tid = nullptr; }
    else         *tid = h;
    return 0;
}

inline int pthread_join(pthread_t tid, void** /*retval*/) {
    if (!tid) return 0;
    WaitForSingleObject(tid, INFINITE);
    CloseHandle(tid);
    return 0;
}

typedef CRITICAL_SECTION pthread_mutex_t;
typedef void* pthread_mutexattr_t;

inline int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t*) {
    InitializeCriticalSection(m); return 0;
}
inline int pthread_mutex_destroy(pthread_mutex_t* m) {
    DeleteCriticalSection(m); return 0;
}
inline int pthread_mutex_lock(pthread_mutex_t* m) {
    EnterCriticalSection(m); return 0;
}
inline int pthread_mutex_unlock(pthread_mutex_t* m) {
    LeaveCriticalSection(m); return 0;
}

typedef CONDITION_VARIABLE pthread_cond_t;
typedef void* pthread_condattr_t;

inline int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t*) {
    InitializeConditionVariable(c); return 0;
}
inline int pthread_cond_destroy(pthread_cond_t* /*c*/) { return 0; }
inline int pthread_cond_signal(pthread_cond_t* c) {
    WakeConditionVariable(c); return 0;
}
inline int pthread_cond_broadcast(pthread_cond_t* c) {
    WakeAllConditionVariable(c); return 0;
}
inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    SleepConditionVariableCS(c, m, INFINITE); return 0;
}
#endif