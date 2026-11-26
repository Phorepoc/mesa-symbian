/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* phe_mutex.c -- OURS: Mesa's mutexes and condition variables (symbian/include/pthread.h routes pthread_mutex_* /
 * pthread_cond_* here). P.I.P.S.' 2010 pthread mutex is an RMutex data lock plus an RSemaphore: several kernel calls
 * per lock/unlock pair, and lock counts itself in the semaphore state before it blocks, so a trylock right after the
 * same thread's unlock can still see EBUSY (composition round 48: every ~1 ms surface-mutex wait was the compositor
 * against itself). Here an uncontended lock/unlock is one atomic each; a P.I.P.S. semaphore (set up on first
 * contention) is used only when a thread really has to sleep. Pure Open C, as the rest of this build. */
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

static sem_t *sem_of(phe_sem_t *s)
{
   int r = __atomic_load_n(&s->ready, __ATOMIC_ACQUIRE);
   if (r == 2)
      return &s->sem;
   int expected = 0;
   if (__atomic_compare_exchange_n(&s->ready, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      sem_init(&s->sem, 0, 0);
      __atomic_store_n(&s->ready, 2, __ATOMIC_RELEASE);
   } else {
      while (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) != 2)
         sched_yield();
   }
   return &s->sem;
}

static void sem_done(phe_sem_t *s)
{
   if (s->ready == 2)
      sem_destroy(&s->sem);
   s->ready = 0;
}

static unsigned self_id(void)
{
   return (unsigned)(unsigned long)pthread_self();
}

int phe_mutex_init(phe_mutex_t *m, const pthread_mutexattr_t *a)
{
   int type = PTHREAD_MUTEX_NORMAL;
   if (a)
      pthread_mutexattr_gettype(a, &type);
   m->state = 0;
   m->owner = 0;
   m->count = 0;
   m->recursive = type == PTHREAD_MUTEX_RECURSIVE;
   m->s.ready = 0;
   return 0;
}

int phe_mutex_destroy(phe_mutex_t *m)
{
   sem_done(&m->s);
   return 0;
}

/* state: 0 free, 1 locked, 2 locked with (possible) sleepers -- the classic three-state futex mutex */
static int take(phe_mutex_t *m, int wait)
{
   unsigned me = 0;
   if (m->recursive) {
      me = self_id();
      if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
         ++m->count;
         return 0;
      }
   }
   int expected = 0;
   if (!__atomic_compare_exchange_n(&m->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      if (!wait)
         return EBUSY;
      sem_t *s = sem_of(&m->s);
      while (__atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE) != 0)
         sem_wait(s);
   }
   if (m->recursive) {
      __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
      m->count = 1;
   }
   return 0;
}

int phe_mutex_lock(phe_mutex_t *m) { return take(m, 1); }
int phe_mutex_trylock(phe_mutex_t *m) { return take(m, 0); }

/* (Mesa's mtx_timedlock) a short sleep between tries: only the test paths take it */
int phe_mutex_timedlock(phe_mutex_t *m, const struct timespec *abs)
{
   for (;;) {
      if (take(m, 0) == 0)
         return 0;
      struct timespec now, nap = {0, 1000000};
      clock_gettime(CLOCK_REALTIME, &now);
      if (now.tv_sec > abs->tv_sec || (now.tv_sec == abs->tv_sec && now.tv_nsec >= abs->tv_nsec))
         return ETIMEDOUT;
      nanosleep(&nap, NULL);
   }
}

int phe_mutex_unlock(phe_mutex_t *m)
{
   if (m->recursive) {
      if (--m->count)
         return 0;
      __atomic_store_n(&m->owner, 0u, __ATOMIC_RELAXED);
   }
   if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
      sem_post(sem_of(&m->s));
   return 0;
}

/* condition variables: a sleeper count + a semaphore. Spurious and stolen wakeups are allowed by POSIX; every Mesa
 * caller waits in a predicate loop. */
int phe_cond_init(phe_cond_t *c, const pthread_condattr_t *a)
{
   (void)a;
   c->waiters = 0;
   c->s.ready = 0;
   return 0;
}

int phe_cond_destroy(phe_cond_t *c)
{
   sem_done(&c->s);
   return 0;
}

static int wake(phe_cond_t *c, int all)
{
   int w = __atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE);
   while (w > 0) {
      const int n = all ? w : 1;
      if (__atomic_compare_exchange_n(&c->waiters, &w, w - n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
         sem_t *s = sem_of(&c->s);
         for (int i = 0; i < n; ++i)
            sem_post(s);
         break;
      }
   }
   return 0;
}

int phe_cond_signal(phe_cond_t *c) { return wake(c, 0); }
int phe_cond_broadcast(phe_cond_t *c) { return wake(c, 1); }

static int wait_for(phe_cond_t *c, phe_mutex_t *m, const struct timespec *abs)
{
   sem_t *s = sem_of(&c->s);
   __atomic_add_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
   /* a recursive mutex is released fully and restored */
   const int depth = m->recursive ? m->count : 0;
   if (depth > 1)
      m->count = 1;
   phe_mutex_unlock(m);
   int r = 0;
   if (!abs) {
      while (sem_wait(s) != 0 && errno == EINTR)
         ;
   } else if (sem_timedwait(s, abs) != 0) {
      /* timed out: take ourselves off the count unless a waker already did (then eat its post) */
      int w = __atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE), removed = 0;
      while (w > 0 && !(removed = __atomic_compare_exchange_n(&c->waiters, &w, w - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)))
         ;
      if (!removed)
         sem_wait(s);
      r = ETIMEDOUT;
   }
   phe_mutex_lock(m);
   if (depth > 1)
      m->count = depth;
   return r;
}

int phe_cond_wait(phe_cond_t *c, phe_mutex_t *m) { return wait_for(c, m, NULL); }
int phe_cond_timedwait(phe_cond_t *c, phe_mutex_t *m, const struct timespec *abs) { return wait_for(c, m, abs); }
