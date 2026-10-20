/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* memstream_symbian.c -- OURS: Mesa's src/util/memstream.c for Symbian (replaces it in build_mesa_symbian.py).
 * P.I.P.S. has no open_memstream (glibc/BSD) and no funopen, so this is Mesa's own Windows scheme: the stream is a
 * tmpfile() and the caller's buffer is refreshed from it on flush and close. struct u_memstream carries the buffer
 * pointers only under _WIN32 (memstream.h), so they live in a small table keyed by the FILE. */
#include "memstream.h"

#include <stdio.h>
#include <stdlib.h>

#include "util/simple_mtx.h"

#define SLOTS 16
static struct {
   FILE *f;
   char **bufp;
   size_t *sizep;
} slots[SLOTS];
static simple_mtx_t slots_mtx = SIMPLE_MTX_INITIALIZER;

bool
u_memstream_open(struct u_memstream *mem, char **bufp, size_t *sizep)
{
   FILE *f = tmpfile();
   if (!f)
      return false;
   simple_mtx_lock(&slots_mtx);
   int i;
   for (i = 0; i < SLOTS && slots[i].f; i++)
      ;
   if (i == SLOTS) {
      simple_mtx_unlock(&slots_mtx);
      fclose(f);
      return false;
   }
   slots[i].f = f;
   slots[i].bufp = bufp;
   slots[i].sizep = sizep;
   simple_mtx_unlock(&slots_mtx);
   *bufp = NULL;
   *sizep = 0;
   mem->f = f;
   return true;
}

static int
slot_of(FILE *f)
{
   for (int i = 0; i < SLOTS; i++)
      if (slots[i].f == f)
         return i;
   return -1;
}

static void
u_memstream_update_buffer(struct u_memstream *mem)
{
   FILE *const f = mem->f;
   simple_mtx_lock(&slots_mtx);
   const int i = slot_of(f);
   char **bufp = i >= 0 ? slots[i].bufp : NULL;
   size_t *sizep = i >= 0 ? slots[i].sizep : NULL;
   simple_mtx_unlock(&slots_mtx);
   if (!bufp)
      return;
   fflush(f);
   long size = ftell(f);
   size_t got = 0;
   if (size > 0) {
      /* reserve space for the null terminator as well */
      if (*bufp == NULL || *sizep < (size_t)size + 1)
         *bufp = realloc(*bufp, size + 1);
      fseek(f, 0, SEEK_SET);
      got = fread(*bufp, 1, size, f);
      (*bufp)[got] = '\0';
      fseek(f, 0, SEEK_END);
   }
   *sizep = got;
}

void
u_memstream_close(struct u_memstream *mem)
{
   u_memstream_update_buffer(mem);
   simple_mtx_lock(&slots_mtx);
   const int i = slot_of(mem->f);
   if (i >= 0)
      slots[i].f = NULL;
   simple_mtx_unlock(&slots_mtx);
   fclose(mem->f);
}

int
u_memstream_flush(struct u_memstream *mem)
{
   u_memstream_update_buffer(mem);
   return 0;
}
