/* HarmonyOS (musl) compatibility for the Android port's native stack. Force-included before every source. */
#pragma once
#include <pthread.h>
/* glibc/bionic static recursive mutex initializer. musl's pthread_mutex_t keeps the mutex type in its first int
   (PTHREAD_MUTEX_RECURSIVE == 1); a zero-initialized remainder is an unlocked mutex. */
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {{{PTHREAD_MUTEX_RECURSIVE}}}
#endif
