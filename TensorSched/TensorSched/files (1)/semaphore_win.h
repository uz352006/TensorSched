#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HANDLE sem_t;
inline int sem_init(sem_t* sem, int /*pshared*/, unsigned int value) {
    *sem = CreateSemaphore(nullptr, (LONG)value, (LONG)value + 100, nullptr);
    return (*sem == nullptr) ? -1 : 0;
}

inline int sem_wait(sem_t* sem) {
    DWORD result = WaitForSingleObject(*sem, INFINITE);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

inline int sem_post(sem_t* sem) {
    return ReleaseSemaphore(*sem, 1, nullptr) ? 0 : -1;
}

inline int sem_destroy(sem_t* sem) {
    return CloseHandle(*sem) ? 0 : -1;
}

#endif // _WIN32
