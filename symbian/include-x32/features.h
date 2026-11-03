/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* features.h -- OURS (x32 build only). The C++ headers of the build host's g++ (libstdc++, glibc configuration; the
 * GLSL compiler uses std::vector) include <features.h> from bits/os_defines.h. Open C has none. Saying "no glibc
 * version" is what that configuration needs: the C library behind these programs is P.I.P.S. */
#ifndef PH_FEATURES_H
#define PH_FEATURES_H
#define __GLIBC_PREREQ(maj, min) 0
#endif
