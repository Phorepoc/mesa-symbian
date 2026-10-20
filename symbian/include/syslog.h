/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* syslog.h -- OURS: Symbian has no syslog. Mesa's util/log.c has a syslog logger for POSIX; on Symbian it is the
 * DEFAULT logger (build_mesa_symbian.py patches log.c) and lands in RDebug through symgl_log (the window glue) --
 * stderr goes nowhere in a Symbian GUI process. Shown in the board log with A64DEFS=-DA64_RDEBUG_LOG. */
#ifndef MESA_SYMBIAN_SYSLOG_H
#define MESA_SYMBIAN_SYSLOG_H

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_USER    (1 << 3)
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_NDELAY  0x08

#ifdef __cplusplus
extern "C" {
#endif
void symgl_syslog(int prio, const char *fmt, ...);   /* tools/mesa/symbian/src/posix_extra.c */
#ifdef __cplusplus
}
#endif

static inline void openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
static inline void closelog(void) {}
#define syslog symgl_syslog

#endif
