// Copyright The Phorepoc Authors
// SPDX-License-Identifier: MIT

// cxx_extra.cpp -- OURS: the libstdc++ error hooks Mesa's header-only std::vector use reaches (glsl
// lower_precision.cpp). Built -fno-exceptions: there is nothing to throw to, so they end the process as libstdc++'s
// own -fno-exceptions build does.
#include <stdlib.h>

namespace std
{
void __throw_length_error(const char *) { abort(); }
void __throw_bad_alloc() { abort(); }
void __throw_bad_array_new_length() { abort(); }
}
