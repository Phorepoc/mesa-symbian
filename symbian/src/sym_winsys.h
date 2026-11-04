/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* sym_winsys.h -- OURS (G2/G4): the Symbian gallium winsys + st frontend (sym_winsys.c, sym_frontend.c). */
#ifndef SYM_WINSYS_H
#define SYM_WINSYS_H

#include "util/format/u_formats.h"
#include "pipe/p_screen.h"
#include "frontend/api.h"
#include "main/menums.h"
#include "symgl_native.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sw_winsys *sym_create_sw_winsys(void);

/* resource_create_front's private: a display target over memory that is not the winsys's own (a window buffer) */
struct sym_dt_memory {
   void *data;
   unsigned stride;
};

/* a framebuffer the state tracker renders into (window: native_window = the RWindow*; pbuffer: NULL) */
struct sym_buffer {
   struct pipe_frontend_drawable base;   /* first: st hands it back as a pipe_frontend_drawable* */
   struct st_visual visual;
   unsigned width, height;               /* allocated */
   unsigned new_width, new_height;       /* wanted (window size / pbuffer size) */
   unsigned mask;
   struct pipe_screen *screen;
   void *native_window;
   int fd;                               /* the Mali (drm_symbian.c) fd, -1 = software */
   enum pipe_texture_target target;
   struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
   /* the window's presentation surface (zero-copy): one colour resource per buffer, BACK_LEFT = slot[cur] */
   int surf_ok;
   symgl_surface surf;
   int prime_fd;                         /* the surface chunk imported for the Mali, -1 = none */
   struct pipe_resource *slot[SYMGL_NBUF];
   int cur;
   int preserve;                         /* EGL_SWAP_BEHAVIOR = EGL_BUFFER_PRESERVED: a present copies the frame forward */
   struct sym_dt_memory dt_mem[SYMGL_NBUF];
   /* G6b: an RSgImage pixmap surface (native_window == NULL): the image's resource is the (front) colour buffer;
    * prime_fd is its import (unimported with the buffer) */
   struct pipe_resource *pixmap_res;
   /* S6: scan-out takeover of a full-screen window on the svga renderer (egl_symbian.c sym_try_scanout) */
   unsigned scanout_off_until;           /* symgl_now_us(): no new attempt before it */
   int fullscreen, fs_check, scanout_on;
   unsigned scanout_since, scanout_backoff;   /* symgl_now_us() of the takeover; the wait after a hand-back (us) */
};

/* 1 when the svga renderer is up: window colour buffers get the scan-out bind (a surface a screen target can show) */
extern int sym_scanout_capable;

struct sym_display {
   struct pipe_frontend_screen *fscreen;
};

/* S5: where the time of a frame goes (microseconds, summed; egl_symbian.c prints them every 120 swaps) */
struct sym_prof { unsigned map, blit, unmap; };
extern struct sym_prof sym_prof;
/* an elapsed time from two symgl_now_us() readings; the CPUs' counters are not in step, so a negative difference counts as 0 */
#define SYM_DUS(a, b) ((int)((b) - (a)) < 0 ? 0u : (unsigned)((b) - (a)))

struct sym_display *sym_create_display(struct pipe_screen *screen);
void sym_destroy_display(struct sym_display *display);

void sym_get_st_visual(struct st_visual *visual, bool alpha, bool depth_stencil);
struct sym_buffer *sym_create_st_framebuffer(struct sym_display *display, const struct st_visual *visual,
                                             void *native_window, int fd);
void sym_destroy_st_framebuffer(struct sym_buffer *buffer);
/* the current back buffer goes to the window; the next one becomes the back buffer (st revalidates) */
void sym_present_st_framebuffer(struct sym_buffer *buffer, struct pipe_context *pipe);

/* api = API_OPENGLES / API_OPENGLES2; major.minor = the EGL context version */
struct st_context *sym_create_context(struct sym_display *display, const struct st_visual *visual,
                                      gl_api api, int major, int minor, struct st_context *shared);
void sym_destroy_context(struct st_context *st);

#ifdef __cplusplus
}
#endif

#endif
