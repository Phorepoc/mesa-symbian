/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* pthread.h -- OURS (tools/mesa/build_mesa_symbian.py): P.I.P.S.' <pthread.h> plus what Mesa uses and P.I.P.S. lacks.
 * P.I.P.S. (2010) has mutexes, condition variables, keys and once -- no barriers, no rwlocks, no
 * pthread_condattr_setclock / pthread_getcpuclockid / pthread_sigmask. Mesa's util/u_thread.h takes pthread barriers
 * whenever HAVE_PTHREAD is set (its own mutex+condvar fallback is only for Apple/Haiku), util/rwlock.h takes
 * pthread rwlocks. Both are provided here over a mutex + condition variable (static inline: no DLL export). */
#ifndef MESA_SYMBIAN_PTHREAD_H
#define MESA_SYMBIAN_PTHREAD_H

#include_next <pthread.h>
#include <errno.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_REALTIME
#endif

/* Mesa's mutexes and condition variables are OURS (symbian/src/phe_mutex.cpp: one atomic per uncontended
 * lock/unlock instead of P.I.P.S.' RMutex + RSemaphore pair). The attribute types stay P.I.P.S.'. */
#include "phe_mutex.h"
#ifdef __cplusplus
extern "C" {
#endif
int phe_mutex_init(phe_mutex_t *m, const pthread_mutexattr_t *a);
int phe_mutex_destroy(phe_mutex_t *m);
int phe_mutex_lock(phe_mutex_t *m);
int phe_mutex_trylock(phe_mutex_t *m);
int phe_mutex_unlock(phe_mutex_t *m);
int phe_cond_init(phe_cond_t *c, const pthread_condattr_t *a);
int phe_cond_destroy(phe_cond_t *c);
int phe_cond_signal(phe_cond_t *c);
int phe_cond_broadcast(phe_cond_t *c);
int phe_cond_wait(phe_cond_t *c, phe_mutex_t *m);
int phe_cond_timedwait(phe_cond_t *c, phe_mutex_t *m, const struct timespec *abs);
#ifdef __cplusplus
}
#endif
#undef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_COND_INITIALIZER
#define pthread_mutex_t phe_mutex_t
#define pthread_cond_t phe_cond_t
#define PTHREAD_MUTEX_INITIALIZER PHE_MUTEX_INITIALIZER
#define PTHREAD_COND_INITIALIZER PHE_COND_INITIALIZER
#define pthread_mutex_init phe_mutex_init
#define pthread_mutex_destroy phe_mutex_destroy
#define pthread_mutex_lock phe_mutex_lock
#define pthread_mutex_trylock phe_mutex_trylock
#define pthread_mutex_unlock phe_mutex_unlock
#define pthread_cond_init phe_cond_init
#define pthread_cond_destroy phe_cond_destroy
#define pthread_cond_signal phe_cond_signal
#define pthread_cond_broadcast phe_cond_broadcast
#define pthread_cond_wait phe_cond_wait
#define pthread_cond_timedwait phe_cond_timedwait

/* condition variables time out against CLOCK_REALTIME only (P.I.P.S.); Mesa's cnd_monotonic asks for monotonic */
static inline int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clk)
{
   (void)attr;
   return clk == CLOCK_REALTIME ? 0 : EINVAL;
}

static inline int pthread_getcpuclockid(pthread_t t, clockid_t *clk)
{
   (void)t; (void)clk;
   return ENOENT;
}

/* no signals between Symbian threads: Mesa's u_thread_create blocks them around pthread_create */
#define pthread_sigmask(how, set, old) ((void)(how), (void)(set), (void)(old), 0)

/* --- barriers --- */
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
typedef struct {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   unsigned count, waiters, sequence;
} pthread_barrier_t;
typedef int pthread_barrierattr_t;

static inline int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
   (void)a;
   if (count == 0)
      return EINVAL;
   pthread_mutex_init(&b->mutex, NULL);
   pthread_cond_init(&b->cond, NULL);
   b->count = count;
   b->waiters = 0;
   b->sequence = 0;
   return 0;
}

static inline int pthread_barrier_destroy(pthread_barrier_t *b)
{
   pthread_cond_destroy(&b->cond);
   pthread_mutex_destroy(&b->mutex);
   return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *b)
{
   pthread_mutex_lock(&b->mutex);
   const unsigned seq = b->sequence;
   if (++b->waiters == b->count) {
      b->waiters = 0;
      b->sequence++;
      pthread_cond_broadcast(&b->cond);
      pthread_mutex_unlock(&b->mutex);
      return PTHREAD_BARRIER_SERIAL_THREAD;
   }
   while (seq == b->sequence)
      pthread_cond_wait(&b->cond, &b->mutex);
   pthread_mutex_unlock(&b->mutex);
   return 0;
}

/* --- read/write locks (writer-preferring) --- */
typedef struct {
   pthread_mutex_t mutex;
   pthread_cond_t readers_ok, writer_ok;
   int readers, writers_waiting, writing;
} pthread_rwlock_t;
typedef int pthread_rwlockattr_t;

static inline int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
   (void)a;
   pthread_mutex_init(&l->mutex, NULL);
   pthread_cond_init(&l->readers_ok, NULL);
   pthread_cond_init(&l->writer_ok, NULL);
   l->readers = l->writers_waiting = l->writing = 0;
   return 0;
}

static inline int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
   pthread_cond_destroy(&l->writer_ok);
   pthread_cond_destroy(&l->readers_ok);
   pthread_mutex_destroy(&l->mutex);
   return 0;
}

static inline int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
   pthread_mutex_lock(&l->mutex);
   while (l->writing || l->writers_waiting)
      pthread_cond_wait(&l->readers_ok, &l->mutex);
   l->readers++;
   pthread_mutex_unlock(&l->mutex);
   return 0;
}

static inline int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
   pthread_mutex_lock(&l->mutex);
   l->writers_waiting++;
   while (l->writing || l->readers)
      pthread_cond_wait(&l->writer_ok, &l->mutex);
   l->writers_waiting--;
   l->writing = 1;
   pthread_mutex_unlock(&l->mutex);
   return 0;
}

static inline int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
   pthread_mutex_lock(&l->mutex);
   if (l->writing)
      l->writing = 0;
   else if (l->readers > 0)
      l->readers--;
   if (l->writers_waiting) {
      if (!l->readers)
         pthread_cond_signal(&l->writer_ok);
   } else {
      pthread_cond_broadcast(&l->readers_ok);
   }
   pthread_mutex_unlock(&l->mutex);
   return 0;
}

#endif
