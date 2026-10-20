/* Copyright 2009 Artur Wyszynski <harakash@gmail.com>
 * Copyright 2013-2014 Alexander von Gluck IV <kallisti5@unixzen.com> (Mesa's hgl_sw_winsys.cpp)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* sym_winsys.c -- OURS (G2): the gallium software winsys for Symbian, after Mesa's Haiku one
 * (src/gallium/winsys/sw/hgl/hgl_sw_winsys.cpp, MIT). A display target is either the winsys's own heap buffer
 * (pbuffers, the no-NGA fallback) or, created through resource_create_front with a sym_dt_memory, memory the window
 * glue owns: one of the window's surface buffers, so softpipe renders straight into what the compositor shows.
 * Presentation is the frontend's job (sym_present_st_framebuffer); "display" here does nothing. */
#include <stdio.h>

#include "pipe/p_defines.h"
#include "util/format/u_formats.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "frontend/sw_winsys.h"

#include "sym_winsys.h"
#include "symgl_native.h"

struct sym_displaytarget {
   enum pipe_format format;
   unsigned width, height, stride, size;
   void *data;
   bool external;
};

static struct sym_displaytarget *
sym_dt(struct sw_displaytarget *dt)
{
   return (struct sym_displaytarget *)dt;
}

static void
sym_winsys_destroy(struct sw_winsys *ws)
{
   FREE(ws);
}

static bool
sym_is_displaytarget_format_supported(struct sw_winsys *ws, unsigned usage, enum pipe_format format)
{
   (void)ws;
   if (usage & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED))
      return format == PIPE_FORMAT_B8G8R8X8_UNORM || format == PIPE_FORMAT_B8G8R8A8_UNORM;
   return true;
}

static struct sw_displaytarget *
sym_displaytarget_create(struct sw_winsys *ws, unsigned usage, enum pipe_format format, unsigned width,
                         unsigned height, unsigned alignment, const void *front_private, unsigned *stride)
{
   (void)ws; (void)usage;
   struct sym_displaytarget *dt = CALLOC_STRUCT(sym_displaytarget);
   if (!dt)
      return NULL;
   dt->format = format;
   dt->width = width;
   dt->height = height;
   if (front_private) {
      const struct sym_dt_memory *mem = front_private;
      dt->stride = mem->stride;
      dt->data = mem->data;
      dt->external = true;
   } else {
      dt->stride = align(util_format_get_stride(format, width), alignment);
      dt->data = align_malloc(dt->stride * util_format_get_nblocksy(format, height), alignment > 16 ? alignment : 16);
      if (!dt->data) {
         FREE(dt);
         return NULL;
      }
   }
   dt->size = dt->stride * util_format_get_nblocksy(format, height);
   *stride = dt->stride;
   return (struct sw_displaytarget *)dt;
}

static void
sym_displaytarget_destroy(struct sw_winsys *ws, struct sw_displaytarget *d)
{
   (void)ws;
   struct sym_displaytarget *dt = sym_dt(d);
   if (!dt)
      return;
   if (!dt->external)
      align_free(dt->data);
   FREE(dt);
}

static struct sw_displaytarget *
sym_displaytarget_from_handle(struct sw_winsys *ws, const struct pipe_resource *templat,
                              struct winsys_handle *whandle, unsigned *stride)
{
   (void)ws; (void)templat; (void)whandle; (void)stride;
   return NULL;
}

static bool
sym_displaytarget_get_handle(struct sw_winsys *ws, struct sw_displaytarget *dt, struct winsys_handle *whandle)
{
   (void)ws; (void)dt; (void)whandle;
   return false;
}

static void *
sym_displaytarget_map(struct sw_winsys *ws, struct sw_displaytarget *dt, unsigned flags)
{
   (void)ws; (void)flags;
   return sym_dt(dt)->data;
}

static void
sym_displaytarget_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   (void)ws; (void)dt;
}

static void
sym_displaytarget_display(struct sw_winsys *ws, struct sw_displaytarget *d, void *context_private,
                          unsigned nboxes, struct pipe_box *box)
{
   (void)ws; (void)d; (void)context_private; (void)nboxes; (void)box;
}

struct sw_winsys *
sym_create_sw_winsys(void)
{
   struct sw_winsys *ws = CALLOC_STRUCT(sw_winsys);
   if (!ws)
      return NULL;
   ws->destroy = sym_winsys_destroy;
   ws->is_displaytarget_format_supported = sym_is_displaytarget_format_supported;
   ws->displaytarget_create = sym_displaytarget_create;
   ws->displaytarget_from_handle = sym_displaytarget_from_handle;
   ws->displaytarget_get_handle = sym_displaytarget_get_handle;
   ws->displaytarget_map = sym_displaytarget_map;
   ws->displaytarget_unmap = sym_displaytarget_unmap;
   ws->displaytarget_display = sym_displaytarget_display;
   ws->displaytarget_destroy = sym_displaytarget_destroy;
   return ws;
}
