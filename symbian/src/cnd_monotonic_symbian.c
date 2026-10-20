/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* cnd_monotonic_symbian.c -- OURS: Mesa's src/util/cnd_monotonic.c for Symbian (replaces it in
 * build_mesa_symbian.py). P.I.P.S. condition variables time out against CLOCK_REALTIME only (no
 * pthread_condattr_setclock), while Mesa hands this API absolute CLOCK_MONOTONIC deadlines. As Mesa does on macOS,
 * the deadline is turned into a relative wait -- here re-anchored on the realtime clock. */
#include "util/cnd_monotonic.h"
#include "util/timespec.h"

#include <assert.h>
#include <errno.h>
#include <time.h>

int
u_cnd_monotonic_init(struct u_cnd_monotonic *cond)
{
   assert(cond != NULL);
   return pthread_cond_init(&cond->cond, NULL) == 0 ? thrd_success : thrd_error;
}

void
u_cnd_monotonic_destroy(struct u_cnd_monotonic *cond)
{
   pthread_cond_destroy(&cond->cond);
}

int
u_cnd_monotonic_broadcast(struct u_cnd_monotonic *cond)
{
   return pthread_cond_broadcast(&cond->cond) == 0 ? thrd_success : thrd_error;
}

int
u_cnd_monotonic_signal(struct u_cnd_monotonic *cond)
{
   return pthread_cond_signal(&cond->cond) == 0 ? thrd_success : thrd_error;
}

int
u_cnd_monotonic_timedwait(struct u_cnd_monotonic *cond, mtx_t *mtx, const struct timespec *abs_time)
{
   struct timespec now_mono, now_real, rel, real_deadline;
   timespec_get(&now_mono, TIME_MONOTONIC);
   clock_gettime(CLOCK_REALTIME, &now_real);
   timespec_sub_saturate(&rel, abs_time, &now_mono);
   timespec_add(&real_deadline, &now_real, &rel);
   int rt = pthread_cond_timedwait(&cond->cond, mtx, &real_deadline);
   if (rt == ETIMEDOUT)
      return thrd_timedout;
   return rt == 0 ? thrd_success : thrd_error;
}

int
u_cnd_monotonic_wait(struct u_cnd_monotonic *cond, mtx_t *mtx)
{
   return pthread_cond_wait(&cond->cond, mtx) == 0 ? thrd_success : thrd_error;
}
