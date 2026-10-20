/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* posix_extra.c -- OURS: C99/POSIX functions Mesa calls that P.I.P.S. (2010) does not export. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "symgl_native.h"

/* Mesa's log (util/log.c syslog logger, the default on Symbian) -> RDebug via the window glue */
void symgl_syslog(int prio, const char *fmt, ...)
{
   char buf[512];
   const int n = snprintf(buf, sizeof(buf), "[mesa:%d] ", prio);
   va_list va;
   va_start(va, fmt);
   vsnprintf(buf + n, sizeof(buf) - n, fmt, va);
   va_end(va);
   symgl_log(buf);
}

double log2(double x)
{
   return log(x) * 1.44269504088896340736;   /* 1 / ln 2 */
}

float log2f(float x)
{
   return logf(x) * 1.44269504088896340736f;
}

/* mkstemp with a fixed suffix after the XXXXXX (util/log.c: MESA_LOG_FILE=...XXXXXX.log) */
int mkstemps(char *tmpl, int suffixlen)
{
   const size_t len = strlen(tmpl);
   if (suffixlen < 0 || len < (size_t)suffixlen + 6) {
      errno = EINVAL;
      return -1;
   }
   char *x = tmpl + len - suffixlen - 6;
   if (memcmp(x, "XXXXXX", 6) != 0) {
      errno = EINVAL;
      return -1;
   }
   static unsigned counter;
   for (int attempt = 0; attempt < 100; attempt++) {
      unsigned v = (unsigned)getpid() * 2654435761u + counter++;
      for (int i = 0; i < 6; i++) {
         x[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36];
         v /= 36;
      }
      int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
      if (fd >= 0 || errno != EEXIST)
         return fd;
   }
   errno = EEXIST;
   return -1;
}
