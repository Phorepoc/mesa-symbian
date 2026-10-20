/* Copyright 2012-2014, Haiku, Inc. (Mesa's Haiku frontend, hgl.c; authors Artur Wyszynski, Alexander von Gluck IV)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* sym_frontend.c -- OURS (G2/G4/G5): the state-tracker frontend for Symbian EGL, adapted from Mesa's Haiku frontend
 * (src/gallium/frontends/hgl/hgl.c, Copyright 2012-2014 Haiku, Inc., MIT; authors Artur Wyszynski, Alexander von
 * Gluck IV). Differences: the context's API and version come from EGL (GLES 1.x / 2.x, not compat GL 1.0), and a
 * window framebuffer renders straight into the window's presentation surface: one colour resource per surface
 * buffer (softpipe: a display target over the buffer's memory; the Mali: the surface chunk imported into the GPU's
 * address space, resource_from_handle with the buffer's offset), the back attachment rotating over them. */
#include <stdio.h>
#include <string.h>

#include "sym_winsys.h"
#include "symgl_native.h"
#include "xf86drm.h"
#include "drm-uapi/drm_fourcc.h"

#include "util/u_atomic.h"
#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "frontend/drm_driver.h"
#include "state_tracker/st_context.h"

static struct sym_buffer *
sym_st_framebuffer(struct pipe_frontend_drawable *drawable)
{
   return (struct sym_buffer *)drawable;
}

static bool
sym_st_framebuffer_flush_front(struct st_context *st, struct pipe_frontend_drawable *drawable,
                               enum st_attachment_type statt)
{
   (void)st; (void)drawable; (void)statt;
   /* presentation is explicit (sym_present_st_framebuffer at eglSwapBuffers); GLES has no front rendering */
   return true;
}

static void
sym_window_slots_destroy(struct sym_buffer *buffer)
{
   for (int s = 0; s < SYMGL_NBUF; s++)
      pipe_resource_reference(&buffer->slot[s], NULL);
   if (buffer->prime_fd >= 0) {
      sym_drm_unimport(buffer->fd, buffer->prime_fd);
      buffer->prime_fd = -1;
   }
   buffer->surf_ok = 0;
}

/* the window's presentation surface at (width, height) and a colour resource over each of its buffers */
static bool
sym_window_slots_create(struct sym_buffer *buffer, unsigned width, unsigned height)
{
   struct pipe_screen *screen = buffer->screen;
   sym_window_slots_destroy(buffer);
   if (!symgl_window_surface(buffer->native_window, width, height, &buffer->surf))
      return false;
   if (buffer->fd >= 0) {
      buffer->prime_fd = sym_drm_import(buffer->fd, buffer->surf.chunk_handle, 0, buffer->surf.chunk_size,
                                        buffer->surf.base);
      if (buffer->prime_fd < 0)
         return false;
   }
   struct pipe_resource templat;
   memset(&templat, 0, sizeof(templat));
   templat.target = buffer->target;
   templat.format = buffer->visual.color_format;
   templat.width0 = width;
   templat.height0 = height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.last_level = 0;
   templat.bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
   for (unsigned s = 0; s < buffer->surf.nbuf; s++) {
      if (buffer->fd >= 0) {
         struct winsys_handle wh;
         memset(&wh, 0, sizeof(wh));
         wh.type = WINSYS_HANDLE_TYPE_FD;
         wh.handle = buffer->prime_fd;
         wh.stride = buffer->surf.stride;
         wh.offset = buffer->surf.offset[s];
         wh.modifier = DRM_FORMAT_MOD_LINEAR;
         wh.format = DRM_FORMAT_XRGB8888;
         buffer->slot[s] = screen->resource_from_handle(screen, &templat, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      } else {
         buffer->dt_mem[s].data = (char *)buffer->surf.base + buffer->surf.offset[s];
         buffer->dt_mem[s].stride = buffer->surf.stride;
         buffer->slot[s] = screen->resource_create_front(screen, &templat, &buffer->dt_mem[s]);
      }
      if (!buffer->slot[s]) {
         char msg[96];
         snprintf(msg, sizeof(msg), "[symgl] window buffer %u: resource creation failed (%s)", s,
                  buffer->fd >= 0 ? "import" : "display target");
         symgl_log(msg);
         sym_window_slots_destroy(buffer);
         return false;
      }
   }
   buffer->surf_ok = 1;
   buffer->cur = symgl_window_acquire(buffer->native_window);
   if (buffer->fd >= 0)
      sym_drm_cache_sync(buffer->fd, buffer->prime_fd, buffer->surf.offset[buffer->cur], buffer->surf.buf_size, 1);
   return true;
}

static bool
sym_st_framebuffer_validate_textures(struct pipe_frontend_drawable *drawable, unsigned width, unsigned height,
                                     unsigned mask)
{
   struct sym_buffer *buffer = sym_st_framebuffer(drawable);
   const bool resized = buffer->width != width || buffer->height != height;
   if (resized) {
      for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++)
         pipe_resource_reference(&buffer->textures[i], NULL);
   }
   if (buffer->native_window && (resized || (!buffer->surf_ok && !buffer->width)))
      sym_window_slots_create(buffer, width, height);   /* (failure = the copy fallback) */

   struct pipe_resource templat;
   memset(&templat, 0, sizeof(templat));
   templat.target = buffer->target;
   templat.width0 = width;
   templat.height0 = height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.last_level = 0;

   for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++) {
      enum pipe_format format = PIPE_FORMAT_NONE;
      unsigned bind = 0;
      if (!((1u << i) & buffer->visual.buffer_mask) || buffer->textures[i])
         continue;
      switch (i) {
      case ST_ATTACHMENT_FRONT_LEFT:
      case ST_ATTACHMENT_BACK_LEFT:
      case ST_ATTACHMENT_FRONT_RIGHT:
      case ST_ATTACHMENT_BACK_RIGHT:
         if (buffer->pixmap_res) {
            /* G6b: an RSgImage pixmap: the image is the colour buffer */
            pipe_resource_reference(&buffer->textures[i], buffer->pixmap_res);
            continue;
         }
         if (buffer->surf_ok) {
            /* the window's buffers: the back attachment is the one acquired for this frame */
            pipe_resource_reference(&buffer->textures[i], buffer->slot[buffer->cur]);
            continue;
         }
         format = buffer->visual.color_format;
         bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET;
         break;
      case ST_ATTACHMENT_DEPTH_STENCIL:
         format = buffer->visual.depth_stencil_format;
         bind = PIPE_BIND_DEPTH_STENCIL;
         break;
      default:
         break;
      }
      if (format != PIPE_FORMAT_NONE) {
         templat.format = format;
         templat.bind = bind;
         buffer->textures[i] = buffer->screen->resource_create(buffer->screen, &templat);
         if (!buffer->textures[i])
            return false;
      }
   }
   buffer->width = width;
   buffer->height = height;
   buffer->mask = mask;
   return true;
}

static bool
sym_st_framebuffer_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
                            const enum st_attachment_type *statts, unsigned count, struct pipe_resource **out,
                            struct pipe_resource **resolve)
{
   (void)st; (void)resolve;
   struct sym_buffer *buffer = sym_st_framebuffer(drawable);
   unsigned mask = 0;
   for (unsigned i = 0; i < count; i++)
      mask |= 1u << statts[i];
   const unsigned new_mask = mask & ~buffer->mask;
   const bool resized = buffer->width != buffer->new_width || buffer->height != buffer->new_height;
   if (resized || new_mask) {
      if (!sym_st_framebuffer_validate_textures(drawable, buffer->new_width, buffer->new_height, mask))
         return false;
   }
   for (unsigned i = 0; i < count; i++)
      pipe_resource_reference(&out[i], buffer->textures[statts[i]]);
   return true;
}

static int
sym_st_manager_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
   (void)fscreen;
   return param == ST_MANAGER_BROKEN_INVALIDATE ? 1 : 0;
}

static uint32_t sym_fb_id;

struct sym_buffer *
sym_create_st_framebuffer(struct sym_display *display, const struct st_visual *visual, void *native_window, int fd)
{
   struct sym_buffer *buffer = CALLOC_STRUCT(sym_buffer);
   if (!buffer)
      return NULL;
   buffer->visual = *visual;
   buffer->screen = display->fscreen->screen;
   buffer->native_window = native_window;
   buffer->fd = fd;
   buffer->prime_fd = -1;
   buffer->target = buffer->screen->caps.npot_textures ? PIPE_TEXTURE_2D : PIPE_TEXTURE_RECT;
   buffer->base.flush_front = sym_st_framebuffer_flush_front;
   buffer->base.validate = sym_st_framebuffer_validate;
   buffer->base.visual = &buffer->visual;
   p_atomic_set(&buffer->base.stamp, 1);
   buffer->base.ID = p_atomic_inc_return(&sym_fb_id);
   buffer->base.fscreen = display->fscreen;
   return buffer;
}

void
sym_destroy_st_framebuffer(struct sym_buffer *buffer)
{
   for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++)
      pipe_resource_reference(&buffer->textures[i], NULL);
   pipe_resource_reference(&buffer->pixmap_res, NULL);
   sym_window_slots_destroy(buffer);
   if (buffer->native_window)
      symgl_window_release(buffer->native_window);
   FREE(buffer);
}

/* the copy fallback: read the back buffer through a transfer and blit it into the window */
static void
sym_present_copy(struct sym_buffer *buffer, struct pipe_context *pipe)
{
   struct pipe_resource *back = buffer->textures[ST_ATTACHMENT_BACK_LEFT];
   if (!back)
      return;
   struct pipe_transfer *transfer = NULL;
   void *map = pipe_texture_map(pipe, back, 0, 0, PIPE_MAP_READ, 0, 0, buffer->width, buffer->height, &transfer);
   if (!map)
      return;
   symgl_window_present_pixels(buffer->native_window, map, transfer->stride, buffer->width, buffer->height);
   pipe_texture_unmap(pipe, transfer);
}

void
sym_present_st_framebuffer(struct sym_buffer *buffer, struct pipe_context *pipe)
{
   if (!buffer->native_window)
      return;
   if (!buffer->surf_ok) {
      sym_present_copy(buffer, pipe);
      return;
   }
   const int done = buffer->cur;
   if (buffer->fd >= 0)   /* the GPU wrote it, the compositor's CPU reads it */
      sym_drm_cache_sync(buffer->fd, buffer->prime_fd, buffer->surf.offset[done], buffer->surf.buf_size, 2);
   symgl_window_present(buffer->native_window, done);
   buffer->cur = symgl_window_acquire(buffer->native_window);
   if (buffer->fd >= 0)   /* the compositor's CPU read it, the GPU will write it */
      sym_drm_cache_sync(buffer->fd, buffer->prime_fd, buffer->surf.offset[buffer->cur], buffer->surf.buf_size, 1);
   /* the next frame's back buffer; st revalidates the framebuffer (the stamp) and binds it */
   pipe_resource_reference(&buffer->textures[ST_ATTACHMENT_BACK_LEFT], buffer->slot[buffer->cur]);
   pipe_resource_reference(&buffer->textures[ST_ATTACHMENT_FRONT_LEFT], buffer->slot[done]);
   p_atomic_inc(&buffer->base.stamp);
}

struct st_context *
sym_create_context(struct sym_display *display, const struct st_visual *visual, gl_api api, int major,
                   int minor, struct st_context *shared)
{
   struct st_context_attribs attribs;
   memset(&attribs, 0, sizeof(attribs));
   attribs.profile = api;
   attribs.major = major;
   attribs.minor = minor;
   attribs.visual = *visual;
   enum st_context_error result = ST_CONTEXT_SUCCESS;
   struct st_context *st = st_api_create_context(display->fscreen, &attribs, &result, shared);
   if (!st) {
      char msg[96];
      snprintf(msg, sizeof(msg), "[symgl] st_api_create_context(api %d, %d.%d) failed: st error %d", (int)api, major,
               minor, (int)result);
      symgl_log(msg);
   }
   return st;
}

void
sym_destroy_context(struct st_context *st)
{
   st_context_flush(st, 0, NULL, NULL, NULL);
   st_destroy_context(st);
}

/* BGRX/BGRA 8888 = Symbian EColor16MU / EColor16MA byte order */
void
sym_get_st_visual(struct st_visual *visual, bool alpha, bool depth_stencil)
{
   memset(visual, 0, sizeof(*visual));
   visual->color_format = alpha ? PIPE_FORMAT_B8G8R8A8_UNORM : PIPE_FORMAT_B8G8R8X8_UNORM;
   visual->depth_stencil_format = depth_stencil ? PIPE_FORMAT_Z24_UNORM_S8_UINT : PIPE_FORMAT_NONE;
   visual->accum_format = PIPE_FORMAT_NONE;
   visual->buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK | ST_ATTACHMENT_BACK_LEFT_MASK;
   if (depth_stencil)
      visual->buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;
}

struct sym_display *
sym_create_display(struct pipe_screen *screen)
{
   struct sym_display *display = CALLOC_STRUCT(sym_display);
   if (!display)
      return NULL;
   display->fscreen = CALLOC_STRUCT(pipe_frontend_screen);
   if (!display->fscreen) {
      FREE(display);
      return NULL;
   }
   display->fscreen->screen = screen;
   display->fscreen->get_param = sym_st_manager_get_param;
   return display;
}

void
sym_destroy_display(struct sym_display *display)
{
   st_screen_destroy(display->fscreen);
   FREE(display->fscreen);
   FREE(display);
}
