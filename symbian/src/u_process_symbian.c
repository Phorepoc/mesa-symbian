/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* u_process_symbian.c -- OURS: Mesa's src/util/u_process.c for Symbian (replaces it in build_mesa_symbian.py).
 * Mesa asks for the process name for per-application workarounds (driconf) and debug output; the Linux/BSD ways
 * (/proc, program_invocation_name, getprogname) do not exist. Symbian's own name (RProcess().Name()) is C++-only,
 * so this answers with a fixed name; per-app options are not used on Symbian. */
#include <string.h>

#include "u_process.h"

const char *
util_get_process_name(void)
{
   return "symbian";
}

size_t
util_get_process_exec_path(char *process_path, size_t len)
{
   (void)process_path;
   (void)len;
   return 0;
}

bool
util_get_command_line(char *cmdline, size_t size)
{
   if (size)
      cmdline[0] = '\0';
   return false;
}
