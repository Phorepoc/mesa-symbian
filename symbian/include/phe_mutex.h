/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* phe_mutex.h -- OURS: the mutex + condition variable Mesa uses on Symbian instead of P.I.P.S.' (the why is in
 * symbian/src/phe_mutex.c). Zero-filled = a free normal mutex / an idle condition variable, so the static
 * initializers stay plain zeros: the semaphore is set up on first contention. */
#ifndef PHE_MUTEX_H
#define PHE_MUTEX_H

#include <semaphore.h>
#include <time.h>

typedef struct phe_sem {
   volatile int ready;      /* 0 not set up, 1 being set up, 2 ready */
   sem_t sem;
} phe_sem_t;

typedef struct phe_mutex {
   volatile int state;      /* 0 free, 1 locked, 2 locked with sleepers */
   volatile unsigned owner; /* recursive only */
   int count;
   int recursive;
   phe_sem_t s;
} phe_mutex_t;

typedef struct phe_cond {
   volatile int waiters;
   phe_sem_t s;
} phe_cond_t;

#define PHE_MUTEX_INITIALIZER {0}
#define PHE_COND_INITIALIZER {0}

#endif
