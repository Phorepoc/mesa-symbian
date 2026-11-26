/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* phe_mutex.h -- OURS: the mutex + condition variable Mesa uses on Symbian instead of P.I.P.S.' (the why is in
 * symbian/src/phe_mutex.cpp). Zero-filled = a free normal mutex / an idle condition variable, so the static
 * initializers stay plain zeros. */
#ifndef PHE_MUTEX_H
#define PHE_MUTEX_H

#include <time.h>

typedef struct phe_mutex {
   volatile int state;      /* 0 free, 1 locked, 2 locked with sleepers */
   volatile int sem;        /* RSemaphore handle, created on first contention */
   volatile unsigned owner; /* recursive only */
   int count;
   int recursive;
} phe_mutex_t;

typedef struct phe_cond {
   volatile int waiters;
   volatile int sem;
} phe_cond_t;

#define PHE_MUTEX_INITIALIZER {0, 0, 0, 0, 0}
#define PHE_COND_INITIALIZER {0, 0}

#endif
