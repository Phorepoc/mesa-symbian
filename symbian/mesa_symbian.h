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

/* Thread-locals: arm-none-eabi GCC compiles __thread as ELF TLS (__aeabi_read_tp + a TLS block per thread), which
 * neither our DLL link nor the kernel sets up, so `thread_local` is compiled away. The variables whose per-thread value
 * matters live in per-thread slots instead (symgl_thread_slot, core's symgl_tls.cpp: Dll::Tls): glapi's current
 * dispatch + context (below: Mesa's own declarations in glapi.h stay valid C, `T *(*f())` declares the accessor),
 * EGL's thread state, qsort_r's comparator and call_once's argument (build_mesa_symbian.py PATCH). A process has
 * several GL threads -- the window server runs the GPU composer and the OpenVG renderer's GL threads -- and as
 * per-process globals one thread's eglMakeCurrent replaced every other thread's context (2026-10-29, GPU mode black).
 * What stays per process: u_debug's and os_misc's formatting scratch buffers (debug output only). */
#define thread_local
#define _Thread_local

#ifdef __cplusplus
extern "C" {
#endif
struct _glapi_table;
struct _glapi_table **sym_glapi_dispatch(void);
void **sym_glapi_context(void);
#ifdef __cplusplus
}
#endif
#define _mesa_glapi_tls_Dispatch (*sym_glapi_dispatch())
#define _mesa_glapi_tls_Context (*sym_glapi_context())

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
