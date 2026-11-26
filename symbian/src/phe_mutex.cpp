// Copyright The Phorepoc Authors
// SPDX-License-Identifier: MIT

// phe_mutex.cpp -- OURS: Mesa's mutexes and condition variables (symbian/include/pthread.h routes pthread_mutex_* /
// pthread_cond_* here). P.I.P.S.' 2010 pthread mutex is an RMutex data lock plus an RSemaphore: four or more kernel
// calls per lock/unlock pair, and lock counts itself in the semaphore state before it blocks, so a trylock right
// after the same thread's unlock can still see EBUSY (composition round 48: every ~1 ms surface-mutex wait was the
// compositor against itself). Here an uncontended lock/unlock is one atomic each; the semaphore (created on first
// contention) is used only when a thread really has to sleep.
#include <e32def.h>	// Mesa's flags drop the Symbian force-include (build_mesa_symbian.py): the base types first
#include <e32std.h>
#include <errno.h>
#include <time.h>

#include <pthread.h>	// OURS: declares the phe_* entry points (symbian/include)

static RSemaphore& Sem(volatile TInt* aHandle)
	{
	// the handle is created at most once per object: the loser of the race closes its own
	if (!__atomic_load_n(aHandle, __ATOMIC_ACQUIRE))
		{
		RSemaphore s;
		if (s.CreateLocal(0) == KErrNone)
			{
			TInt expected = 0;
			if (!__atomic_compare_exchange_n(aHandle, &expected, s.Handle(), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				s.Close();
			}
		}
	return *(RSemaphore*)aHandle;	// RSemaphore is an RHandleBase: one TInt handle
	}

static unsigned Self()
	{
	return (unsigned)RThread().Id();
	}

extern "C" int phe_mutex_init(phe_mutex_t* m, const pthread_mutexattr_t* a)
	{
	int type = PTHREAD_MUTEX_NORMAL;
	if (a)
		pthread_mutexattr_gettype(a, &type);
	m->state = 0; m->sem = 0; m->owner = 0; m->count = 0;
	m->recursive = (type == PTHREAD_MUTEX_RECURSIVE);
	return 0;
	}

extern "C" int phe_mutex_destroy(phe_mutex_t* m)
	{
	if (m->sem)
		{
		Sem(&m->sem).Close();
		m->sem = 0;
		}
	return 0;
	}

// state: 0 free, 1 locked, 2 locked with (possible) sleepers -- the classic three-state futex mutex
static void LockSlow(phe_mutex_t* m)
	{
	RSemaphore& s = Sem(&m->sem);
	while (__atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE) != 0)
		s.Wait();
	}

extern "C" int phe_mutex_lock(phe_mutex_t* m)
	{
	unsigned me = 0;
	if (m->recursive)
		{
		me = Self();
		if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me)
			{
			++m->count;
			return 0;
			}
		}
	int expected = 0;
	if (!__atomic_compare_exchange_n(&m->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		LockSlow(m);
	if (m->recursive)
		{
		__atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
		m->count = 1;
		}
	return 0;
	}

// (Mesa's mtx_timedlock) a short sleep between tries: only the GLX/test paths take it
extern "C" int phe_mutex_timedlock(phe_mutex_t* m, const struct timespec* aAbs)
	{
	for (;;)
		{
		if (phe_mutex_trylock(m) == 0)
			return 0;
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		if (now.tv_sec > aAbs->tv_sec || (now.tv_sec == aAbs->tv_sec && now.tv_nsec >= aAbs->tv_nsec))
			return ETIMEDOUT;
		User::AfterHighRes(1000);
		}
	}

extern "C" int phe_mutex_trylock(phe_mutex_t* m)
	{
	unsigned me = 0;
	if (m->recursive)
		{
		me = Self();
		if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me)
			{
			++m->count;
			return 0;
			}
		}
	int expected = 0;
	if (!__atomic_compare_exchange_n(&m->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return EBUSY;
	if (m->recursive)
		{
		__atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
		m->count = 1;
		}
	return 0;
	}

extern "C" int phe_mutex_unlock(phe_mutex_t* m)
	{
	if (m->recursive)
		{
		if (--m->count)
			return 0;
		__atomic_store_n(&m->owner, 0u, __ATOMIC_RELAXED);
		}
	if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
		Sem(&m->sem).Signal();
	return 0;
	}

// condition variables: a sleeper count + a semaphore. Spurious and stolen wakeups are allowed by POSIX; every
// Mesa caller waits in a predicate loop.
extern "C" int phe_cond_init(phe_cond_t* c, const pthread_condattr_t*)
	{
	c->waiters = 0; c->sem = 0;
	return 0;
	}

extern "C" int phe_cond_destroy(phe_cond_t* c)
	{
	if (c->sem)
		{
		Sem(&c->sem).Close();
		c->sem = 0;
		}
	return 0;
	}

static int Wake(phe_cond_t* c, bool aAll)
	{
	int w = __atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE);
	while (w > 0)
		{
		const int n = aAll ? w : 1;
		if (__atomic_compare_exchange_n(&c->waiters, &w, w - n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			{
			Sem(&c->sem).Signal(n);
			break;
			}
		}
	return 0;
	}

extern "C" int phe_cond_signal(phe_cond_t* c) { return Wake(c, false); }
extern "C" int phe_cond_broadcast(phe_cond_t* c) { return Wake(c, true); }

static int WaitFor(phe_cond_t* c, phe_mutex_t* m, TInt aTimeoutUs)
	{
	RSemaphore& s = Sem(&c->sem);
	__atomic_add_fetch(&c->waiters, 1, __ATOMIC_ACQ_REL);
	// a recursive mutex is released fully and restored, as POSIX requires for a single-level hold
	const int depth = m->recursive ? m->count : 0;
	if (depth > 1)
		m->count = 1;
	phe_mutex_unlock(m);
	int r = 0;
	if (aTimeoutUs < 0)
		s.Wait();
	else if (s.Wait(aTimeoutUs ? aTimeoutUs : 1) != KErrNone)
		{
		// timed out: take ourselves off the count unless a waker already did (then eat its signal)
		int w = __atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE);
		bool removed = false;
		while (w > 0 && !(removed = __atomic_compare_exchange_n(&c->waiters, &w, w - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)))
			;
		if (!removed)
			s.Wait();
		r = ETIMEDOUT;
		}
	phe_mutex_lock(m);
	if (depth > 1)
		m->count = depth;
	return r;
	}

extern "C" int phe_cond_wait(phe_cond_t* c, phe_mutex_t* m)
	{
	return WaitFor(c, m, -1);
	}

extern "C" int phe_cond_timedwait(phe_cond_t* c, phe_mutex_t* m, const struct timespec* aAbs)
	{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	long long us = (long long)(aAbs->tv_sec - now.tv_sec) * 1000000 + (aAbs->tv_nsec - now.tv_nsec) / 1000;
	if (us <= 0)
		us = 0;
	if (us > 0x7fffffff)
		us = 0x7fffffff;
	return WaitFor(c, m, (TInt)us);
	}
