#pragma once
#ifdef _WIN32
  #include "pthread_win.h"
  #include "semaphore_win.h"
  #include <windows.h>
  inline void usleep(unsigned long us) { Sleep(us / 1000); }
#else
  #include <pthread.h>
  #include <semaphore.h>
  #include <unistd.h>
#endif