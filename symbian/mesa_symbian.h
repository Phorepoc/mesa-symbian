/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* mesa_symbian.h -- force-included into every Mesa TU built for Symbian (tools/mesa/build_mesa_symbian.py). OURS.
 *
 * Mesa's util/detect_os.h knows Linux, the BSDs, Haiku, Fuchsia, Windows ... -- not Symbian, so every DETECT_OS_*
 * would be 0 and the POSIX paths (pthread threads, clock_gettime, getenv) would be compiled out. P.I.P.S. (the
 * Symbian POSIX layer: libc/libpthread/libm, stdapis headers) provides those, so Symbian counts as POSIX here.
 * Facts P.I.P.S. lacks stay off (HAVE_* macros dropped by the build script's GLOBAL_DROP).
 */
#ifndef MESA_SYMBIAN_H
#define MESA_SYMBIAN_H

#define DETECT_OS_SYMBIAN 1
#define DETECT_OS_POSIX 1

/* util/u_endian.h finds no <endian.h> on Symbian; ARM Symbian is little-endian */
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN 0

/* Thread-locals (G2 first light): arm-none-eabi GCC compiles __thread as ELF TLS (__aeabi_read_tp + a TLS block per
 * thread), which neither our DLL link nor the kernel sets up. Mesa has a handful (glapi's current dispatch + context,
 * EGL's per-thread state, u_debug/call_once scratch): they become per-process globals (DLL WSD is per process). That
 * is right for the usual Symbian GL client -- one GL thread per process -- and WRONG for two threads with current
 * contexts in one process. The fix when that matters: real TLS (an __aeabi_read_tp that returns a per-thread block
 * built from .tdata/.tbss, kept by the DLL linker script). */
#define thread_local
#define _Thread_local

/* C11 bits the 2010 stdapis headers predate */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
#ifndef alloca
#define alloca __builtin_alloca
#endif
#ifndef _GCC_MAX_ALIGN_T
#define _GCC_MAX_ALIGN_T
typedef struct {
   long long __max_align_ll __attribute__((__aligned__(__alignof__(long long))));
   long double __max_align_ld __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
#endif

/* sysconf name P.I.P.S. does not know (-> -1: os_get_total_physical_memory reports failure, Mesa copes) */
#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 0x7ff0
#endif

/* libm + libc gaps (tools/mesa/symbian/src/posix_extra.c) */
#ifdef __cplusplus
extern "C" {
#endif
double log2(double);
float log2f(float);
int mkstemps(char *tmpl, int suffixlen);
#ifdef __cplusplus
}
#endif

#endif
