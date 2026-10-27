/* Copyright (C) 2014 Adrián Arroyo Calle <adrian.arroyocalle@gmail.com> (Mesa's Haiku EGL driver, egl_haiku.cpp)
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* egl_symbian.c -- OURS (G2/G4): Mesa's EGL driver for Symbian, a C port of the Haiku driver
 * (src/egl/drivers/haiku/egl_haiku.cpp, Copyright (C) 2014 Adrián Arroyo Calle, MIT) onto sym_frontend.c +
 * sym_winsys.c. A window surface is an RWindow* (EGLNativeWindowType). The pipe screen is the Mali-400 through
 * Mesa's lima driver over our kernel driver (drm_symbian.c) when the kernel has one, else softpipe
 * (SYMGL_SOFTPIPE=1 in the environment forces softpipe). Built with _EGL_NATIVE_PLATFORM=_EGL_PLATFORM_HAIKU:
 * Mesa's EGL core treats that platform's native windows as opaque pointers passed straight to the driver -- exactly
 * the Symbian semantics (no Symbian enum in egldisplay.h; the name only shows in EGL_PLATFORM debug output).
 * Contexts: OpenGL ES 1.x (API_OPENGLES) and 2.x (API_OPENGLES2). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "egllog.h"
#include "eglsurface.h"
#include "eglsync.h"
#include "egltypedefs.h"
#include "c11/threads.h"
#include "util/os_time.h"
#include <time.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "main/glthread.h"

#include "sym_winsys.h"
#include "symgl_native.h"
#include "xf86drm.h"
#include "target-helpers/inline_sw_helper.h"

#ifdef GALLIUM_LIMA
struct renderonly;
struct pipe_screen *lima_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro);
#endif

_EGL_DRIVER_STANDARD_TYPECASTS(sym_egl)

struct sym_egl_display {
   int ref_count;
   struct sym_display *disp;
   int fd;                      /* the Mali, -1 = softpipe */
};

struct sym_egl_config {
   _EGLConfig base;
};

struct sym_egl_context {
   _EGLContext base;
   struct st_context *st;
};

struct sym_egl_surface {
   _EGLSurface base;
   struct sym_buffer *fb;
   int range;                   /* G6b: cache-sync registry slot of a pixmap surface, -1 = none */
};

static void sym_range_remove(int idx);

static void
update_size(struct sym_buffer *buffer)
{
   unsigned w, h;
   if (!buffer->native_window || !symgl_window_size(buffer->native_window, &w, &h))
      return;
   if (buffer->new_width != w || buffer->new_height != h) {
      buffer->new_width = w;
      buffer->new_height = h;
      p_atomic_inc(&buffer->base.stamp);
   }
}

static struct st_visual
visual_for(const _EGLConfig *conf)
{
   struct st_visual v;
   sym_get_st_visual(&v, conf && conf->AlphaSize > 0, !conf || conf->DepthSize > 0 || conf->StencilSize > 0);
   if (conf && conf->RedSize == 5)
      v.color_format = PIPE_FORMAT_B5G6R5_UNORM;
   return v;
}

static _EGLSurface *
sym_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf, void *native_window, const EGLint *attrib_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   unsigned w, h;
   if (!native_window || !symgl_window_size(native_window, &w, &h)) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
      return NULL;
   }
   struct sym_egl_surface *surf = calloc(1, sizeof(*surf));
   if (!surf)
      return NULL;
   surf->range = -1;
   if (!_eglInitSurface(&surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list, native_window)) {
      free(surf);
      return NULL;
   }
   struct st_visual visual = visual_for(conf);
   surf->fb = sym_create_st_framebuffer(dpy->disp, &visual, native_window, dpy->fd);
   if (!surf->fb) {
      free(surf);
      return NULL;
   }
   surf->base.Width = w;
   surf->base.Height = h;
   update_size(surf->fb);
   return &surf->base;
}

static _EGLSurface *sym_create_sgimage_surface(_EGLDisplay *disp, _EGLConfig *conf, void *rsgimage,
                                               const EGLint *attrib_list);

/* EGLNativePixmapType = RSgImage* (EGL_SYMBIAN_PIXMAP_TYPE_RSGIMAGE / EGL_NOK_pixmap_type_rsgimage); see G6b below */
static _EGLSurface *
sym_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf, void *native_pixmap, const EGLint *attrib_list)
{
   return sym_create_sgimage_surface(disp, conf, native_pixmap, attrib_list);
}

static _EGLSurface *
sym_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf, const EGLint *attrib_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   struct sym_egl_surface *surf = calloc(1, sizeof(*surf));
   if (!surf)
      return NULL;
   surf->range = -1;
   if (!_eglInitSurface(&surf->base, disp, EGL_PBUFFER_BIT, conf, attrib_list, NULL)) {
      free(surf);
      return NULL;
   }
   struct st_visual visual = visual_for(conf);
   surf->fb = sym_create_st_framebuffer(dpy->disp, &visual, NULL, dpy->fd);
   if (!surf->fb) {
      free(surf);
      return NULL;
   }
   surf->fb->new_width = surf->base.Width;
   surf->fb->new_height = surf->base.Height;
   p_atomic_inc(&surf->fb->base.stamp);
   return &surf->base;
}

static EGLBoolean
sym_destroy_surface(_EGLDisplay *disp, _EGLSurface *s)
{
   (void)disp;
   if (_eglPutSurface(s)) {
      struct sym_egl_surface *surf = sym_egl_surface(s);
      sym_range_remove(surf->range);
      sym_destroy_st_framebuffer(surf->fb);
      free(surf);
   }
   return EGL_TRUE;
}

static EGLBoolean
sym_swap_buffers(_EGLDisplay *disp, _EGLSurface *s)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   struct sym_egl_surface *surf = sym_egl_surface(s);
   struct sym_egl_context *ctx = sym_egl_context(s->CurrentContext);
   if (!ctx)
      return EGL_FALSE;

   struct st_context *st = ctx->st;
   struct pipe_screen *screen = dpy->disp->fscreen->screen;
   struct sym_buffer *buffer = surf->fb;

   if (s->Type != EGL_WINDOW_BIT) {      /* pixmap / pbuffer: eglSwapBuffers is a no-op (EGL 1.4 3.9.1) */
      st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);
      return EGL_TRUE;
   }
   _mesa_glthread_finish(st->ctx);
   /* the frame is complete in its buffer before the compositor gets it: flush, then wait for the GPU */
   struct pipe_fence_handle *fence = NULL;
   st_context_flush(st, ST_FLUSH_FRONT, &fence, NULL, NULL);
   if (fence) {
      screen->fence_finish(screen, NULL, fence, OS_TIMEOUT_INFINITE);
      screen->fence_reference(screen, &fence, NULL);
   }
   sym_present_st_framebuffer(buffer, st->pipe);
   update_size(buffer);
   st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);
   return EGL_TRUE;
}

static EGLBoolean
sym_add_config(_EGLDisplay *disp, EGLint id, EGLint alpha, EGLint rgb565)
{
   struct sym_egl_config *conf = calloc(1, sizeof(*conf));
   if (!conf)
      return _eglError(EGL_BAD_ALLOC, "sym_add_config");
   _eglInitConfig(&conf->base, disp, id);
   conf->base.RedSize = rgb565 ? 5 : 8;
   conf->base.GreenSize = rgb565 ? 6 : 8;
   conf->base.BlueSize = rgb565 ? 5 : 8;
   conf->base.AlphaSize = alpha;
   conf->base.LuminanceSize = 0;
   conf->base.ColorBufferType = EGL_RGB_BUFFER;
   conf->base.BufferSize = rgb565 ? 16 : 24 + alpha;
   conf->base.ConfigCaveat = EGL_NONE;
   conf->base.ConfigID = id;
   conf->base.BindToTextureRGB = EGL_FALSE;
   conf->base.BindToTextureRGBA = EGL_FALSE;
   conf->base.DepthSize = 24;
   conf->base.StencilSize = 8;
   conf->base.TransparentType = EGL_NONE;
   conf->base.NativeRenderable = EGL_TRUE;
   conf->base.NativeVisualID = 0;
   conf->base.NativeVisualType = EGL_NONE;
   conf->base.RenderableType = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
   conf->base.Conformant = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
   conf->base.SampleBuffers = 0;
   conf->base.Samples = 0;
   conf->base.Level = 0;
   conf->base.MaxPbufferWidth = _EGL_MAX_PBUFFER_WIDTH;
   conf->base.MaxPbufferHeight = _EGL_MAX_PBUFFER_HEIGHT;
   conf->base.MaxPbufferPixels = _EGL_MAX_PBUFFER_WIDTH * _EGL_MAX_PBUFFER_HEIGHT;
   /* G6b: pixmaps are RSgImages; the 16 bit config exists for RGB_565 images only (the window buffers are 32 bit) */
   conf->base.SurfaceType = rgb565 ? EGL_PIXMAP_BIT : EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_PIXMAP_BIT;
   if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
      _eglLog(_EGL_WARNING, "symbian: config %d did not validate", id);
      free(conf);
      return EGL_FALSE;
   }
   _eglLinkConfig(&conf->base);
   return EGL_TRUE;
}

static void
sym_display_destroy(_EGLDisplay *disp)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   if (!dpy || !p_atomic_dec_zero(&dpy->ref_count))
      return;
   struct pipe_screen *screen = dpy->disp->fscreen->screen;
   sym_destroy_display(dpy->disp);
   screen->destroy(screen);
   if (dpy->fd >= 0)
      sym_drm_close(dpy->fd);
   free(dpy);
   disp->DriverData = NULL;
}

static struct pipe_screen *
sym_create_screen(int *fd)
{
   *fd = -1;
#ifdef GALLIUM_LIMA
   if (!getenv("SYMGL_SOFTPIPE")) {
      *fd = sym_drm_open();
      if (*fd >= 0) {
         struct pipe_screen_config cfg;
         memset(&cfg, 0, sizeof(cfg));
         struct pipe_screen *screen = lima_screen_create(*fd, &cfg, NULL);
         if (screen) {
            symgl_log("[symgl] eglInitialize: lima (Mali-400) screen up");
            return screen;
         }
         symgl_log("[symgl] eglInitialize: lima_screen_create failed -- softpipe");
         sym_drm_close(*fd);
         *fd = -1;
      }
   }
#endif
   struct sw_winsys *winsys = sym_create_sw_winsys();
   struct pipe_screen *screen = winsys ? sw_screen_create(winsys) : NULL;
   if (screen)
      symgl_log("[symgl] eglInitialize: softpipe screen up");
   return screen;
}

/* ---------------------------------------------------------------------------------------------------------------
 * G6: EGLImage from a shared chunk -- EGL_EXT_image_dma_buf_import with Symbian semantics: a plane "fd" is an RChunk
 * HANDLE of the calling process (a SurfaceManager surface, the VE's frame arena, ...), offsets/pitches inside it.
 * Formats: DRM_FORMAT_XRGB8888 / ARGB8888 (one plane) and DRM_FORMAT_NV12 (two planes of one chunk: R8 + R8G8 resources,
 * the state tracker samples them as one external YUV texture -- GL_OES_EGL_image_external, BT.601 narrow by default).
 * The chunk is imported for the GPU once per image (RMaliChannel::Import) and unimported with it. */
#include "eglimage.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/u_inlines.h"
#include "frontend/drm_driver.h"
#include "frontend/api.h"

#define SYM_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define SYM_FMT_XRGB8888 SYM_FOURCC('X', 'R', '2', '4')
#define SYM_FMT_ARGB8888 SYM_FOURCC('A', 'R', '2', '4')
#define SYM_FMT_NV12 SYM_FOURCC('N', 'V', '1', '2')
#define SYM_FMT_R8 SYM_FOURCC('R', '8', ' ', ' ')
#define SYM_FMT_GR88 SYM_FOURCC('G', 'R', '8', '8')


struct sym_egl_image {
   _EGLImage base;
   struct pipe_resource *tex;   /* plane 0; plane 1 hangs off ->next */
   enum pipe_format format;     /* NV12 for two planes, else the texture's own */
   unsigned internalformat;
   int fd;                      /* the Mali device */
   int prime_fd;                /* the chunk's import */
   int range;                   /* G6b: cache-sync registry slot (RSgImage images), -1 = none */
};

static _EGLDisplay *sym_the_disp;

static _EGLImage *sym_create_sgimage_image(_EGLDisplay *disp, EGLClientBuffer rsgimage, const EGLint *attr_list);
/* G8: eglCreateImageKHR(EGL_SYMBIAN_MEMORY_IMAGE_PH, (symgl_sgimage*)desc) -- a sampler / render-target image over memory
 * of this process that lies in a SHARED chunk (the composer's surface buffers), imported by address like an RSgImage */
#define EGL_SYMBIAN_MEMORY_IMAGE_PH 0x3FF1
static const EGLint sym_mem_attr_tag[] = {EGL_NONE};
static int sym_range_add(int prime, uint32_t off, uint32_t size);

static _EGLImage *
sym_create_image(_EGLDisplay *disp, _EGLContext *ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attr_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   (void)ctx;
   if (target == EGL_NATIVE_PIXMAP_KHR && dpy && dpy->fd >= 0)
      return sym_create_sgimage_image(disp, buffer, attr_list);
   if (target == EGL_SYMBIAN_MEMORY_IMAGE_PH && dpy && dpy->fd >= 0)
      return sym_create_sgimage_image(disp, buffer, sym_mem_attr_tag);
   if (target != EGL_LINUX_DMA_BUF_EXT || !dpy || dpy->fd < 0) {
      _eglError(EGL_BAD_PARAMETER,
                "eglCreateImageKHR: only EGL_LINUX_DMA_BUF_EXT (a chunk handle) or EGL_NATIVE_PIXMAP_KHR (RSgImage*) on the Mali");
      return NULL;
   }
   _EGLImageAttribs a;
   if (!_eglParseImageAttribList(&a, disp, attr_list))
      return NULL;
   const uint32_t fourcc = (uint32_t)a.DMABufFourCC.Value;
   unsigned nplanes = 1;
   enum pipe_format pf[2] = {PIPE_FORMAT_NONE, PIPE_FORMAT_NONE}, fmt;
   uint32_t pfourcc[2] = {fourcc, 0};
   unsigned internalformat = 0x8051; /* GL_RGB8 */
   if (fourcc == SYM_FMT_NV12) {
      nplanes = 2;
      pf[0] = PIPE_FORMAT_R8_UNORM, pf[1] = PIPE_FORMAT_R8G8_UNORM, fmt = PIPE_FORMAT_NV12;
      pfourcc[0] = SYM_FMT_R8, pfourcc[1] = SYM_FMT_GR88;
   } else if (fourcc == SYM_FMT_XRGB8888) {
      pf[0] = fmt = PIPE_FORMAT_B8G8R8X8_UNORM;
   } else if (fourcc == SYM_FMT_ARGB8888) {
      pf[0] = fmt = PIPE_FORMAT_B8G8R8A8_UNORM;
      internalformat = 0x8058; /* GL_RGBA8 */
   } else {
      _eglError(EGL_BAD_MATCH, "eglCreateImageKHR: format not supported");
      return NULL;
   }
   if (a.Width <= 0 || a.Height <= 0 || !a.DMABufPlaneFds[0].IsPresent ||
       (nplanes == 2 && a.DMABufPlaneFds[1].IsPresent && a.DMABufPlaneFds[1].Value != a.DMABufPlaneFds[0].Value)) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateImageKHR: size, or the planes are not in one chunk");
      return NULL;
   }
   const int handle = a.DMABufPlaneFds[0].Value;
   const unsigned size = symgl_chunk_size(handle);
   const int prime = size ? sym_drm_import(dpy->fd, handle, 0, size, NULL) : -1;
   if (prime < 0) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateImageKHR: the chunk cannot be imported");
      return NULL;
   }
   struct pipe_screen *screen = dpy->disp->fscreen->screen;
   struct pipe_resource *res[2] = {NULL, NULL};
   for (unsigned p = 0; p < nplanes; p++) {
      struct pipe_resource templat;
      memset(&templat, 0, sizeof(templat));
      templat.target = PIPE_TEXTURE_2D;
      templat.format = pf[p];
      templat.width0 = p ? (a.Width + 1) / 2 : a.Width;
      templat.height0 = p ? (a.Height + 1) / 2 : a.Height;
      templat.depth0 = 1;
      templat.array_size = 1;
      templat.bind = PIPE_BIND_SAMPLER_VIEW;
      struct winsys_handle wh;
      memset(&wh, 0, sizeof(wh));
      wh.type = WINSYS_HANDLE_TYPE_FD;
      wh.handle = prime;
      wh.offset = a.DMABufPlaneOffsets[p].Value;
      wh.stride = a.DMABufPlanePitches[p].Value;
      wh.modifier = DRM_FORMAT_MOD_LINEAR;
      wh.format = pfourcc[p];
      res[p] = screen->resource_from_handle(screen, &templat, &wh, 0);
      if (!res[p])
         break;
   }
   if (!res[0] || (nplanes == 2 && !res[1])) {
      pipe_resource_reference(&res[1], NULL);
      pipe_resource_reference(&res[0], NULL);
      sym_drm_unimport(dpy->fd, prime);
      _eglError(EGL_BAD_ALLOC, "eglCreateImageKHR: resource_from_handle failed");
      return NULL;
   }
   res[0]->next = res[1];          /* the chain owns plane 1 (as the DRI frontend does) */
   struct sym_egl_image *img = calloc(1, sizeof(*img));
   if (!img) {
      pipe_resource_reference(&res[0], NULL);
      sym_drm_unimport(dpy->fd, prime);
      _eglError(EGL_BAD_ALLOC, "eglCreateImageKHR");
      return NULL;
   }
   _eglInitImage(&img->base, disp);
   img->tex = res[0];
   img->format = fmt;
   img->internalformat = internalformat;
   img->fd = dpy->fd;
   img->prime_fd = prime;
   img->range = -1;
   return &img->base;
}

static EGLBoolean
sym_destroy_image(_EGLDisplay *disp, _EGLImage *image)
{
   struct sym_egl_image *img = (struct sym_egl_image *)image;
   (void)disp;
   sym_range_remove(img->range);
   pipe_resource_reference(&img->tex, NULL);    /* both planes */
   sym_drm_unimport(img->fd, img->prime_fd);
   free(img);
   return EGL_TRUE;
}

static bool
sym_validate_egl_image(struct pipe_frontend_screen *fscreen, void *egl_image)
{
   (void)fscreen;
   return sym_the_disp && _eglLookupImage((EGLImage)egl_image, sym_the_disp) != NULL;
}

static bool
sym_get_egl_image(struct pipe_frontend_screen *fscreen, void *egl_image, struct st_egl_image *out)
{
   (void)fscreen;
   _EGLImage *i = sym_the_disp ? _eglLookupImage((EGLImage)egl_image, sym_the_disp) : NULL;
   if (!i)
      return false;
   struct sym_egl_image *img = (struct sym_egl_image *)i;
   memset(out, 0, sizeof(*out));
   pipe_resource_reference(&out->texture, img->tex);
   out->format = img->format;
   out->level = 0;
   out->layer = 0;
   out->internalformat = img->internalformat;
   return true;
}

/* ---------------------------------------------------------------------------------------------------------------
 * G6b: RSgImage -- the 2010 graphics resource as EGL pixmaps and EGLImages (EGL_KHR_image_pixmap,
 * EGL_SYMBIAN_PIXMAP_TYPE_RSGIMAGE / EGL_NOK_pixmap_type_rsgimage). An RSgImage's pixels are a shared chunk of the
 * sgresource kernel extension; symgl_sgimage_info() turns the handle into (CPU address, stride, format) and the
 * chunk's pages are imported into the Mali's address space by that address (sym_drm_import_addr) -- zero copy:
 *   - eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR, RSgImage*) = a sampler/render-target resource over the image
 *     (glEGLImageTargetTexture2DOES / glEGLImageTargetRenderbufferStorageOES);
 *   - eglCreatePixmapSurface(RSgImage*) = a single-buffered surface whose colour buffer IS the image (softpipe: a
 *     display target over the chunk's CPU address).
 * CPU cache maintenance (the image's chunk is cached when the image has CPU access): the imported ranges are
 * registered; eglWaitNative = the CPU's writes go to memory before the GPU reads/writes (clean+invalidate),
 * eglWaitClient = finish the GPU, then invalidate so the CPU reads what the GPU wrote (RMaliChannel::CacheSync ops). */

#define SYM_MAX_RANGES 32
static struct {
   int used, prime;
   uint32_t off, size;
} sym_ranges[SYM_MAX_RANGES];

static int
sym_range_add(int prime, uint32_t off, uint32_t size)
{
   for (int i = 0; i < SYM_MAX_RANGES; i++) {
      if (!sym_ranges[i].used) {
         sym_ranges[i].used = 1;
         sym_ranges[i].prime = prime;
         sym_ranges[i].off = off;
         sym_ranges[i].size = size;
         return i;
      }
   }
   return -1;   /* (no cache maintenance for it; every use still works on uncached images) */
}

static void
sym_range_remove(int idx)
{
   if (idx >= 0 && idx < SYM_MAX_RANGES)
      sym_ranges[idx].used = 0;
}

static void
sym_ranges_sync(int fd, int op)
{
   for (int i = 0; i < SYM_MAX_RANGES; i++)
      if (sym_ranges[i].used)
         sym_drm_cache_sync(fd, sym_ranges[i].prime, sym_ranges[i].off, sym_ranges[i].size, op);
}

struct sym_sg_layout {
   symgl_sgimage sg;
   enum pipe_format pf;
   uint32_t drm_fourcc;
   unsigned internalformat;
   void *imp_addr;                        /* the import: page aligned, a page multiple of bytes */
   unsigned imp_size, res_off;            /* ... and the image's offset inside it */
};

static bool sym_sg_layout_fill(struct sym_sg_layout *l);

static bool
sym_sg_layout_get(void *rsgimage, struct sym_sg_layout *l)
{
   memset(l, 0, sizeof(*l));
   if (!symgl_sgimage_info(rsgimage, &l->sg))
      return false;
   return sym_sg_layout_fill(l);
}

/* G8: plain memory of this process described directly (EGL_SYMBIAN_MEMORY_IMAGE_PH; the buffer IS a symgl_sgimage*) */
static bool
sym_mem_layout_get(const symgl_sgimage *mem, struct sym_sg_layout *l)
{
   memset(l, 0, sizeof(*l));
   if (!mem || !mem->base || !mem->width || !mem->height)
      return false;
   l->sg = *mem;
   return sym_sg_layout_fill(l);
}

static bool
sym_sg_layout_fill(struct sym_sg_layout *l)
{
   switch (l->sg.format) {
   case SYMGL_SGFMT_ARGB8888:
      l->pf = PIPE_FORMAT_B8G8R8A8_UNORM;
      l->drm_fourcc = DRM_FORMAT_ARGB8888;
      l->internalformat = 0x8058;   /* GL_RGBA8 */
      break;
   case SYMGL_SGFMT_XRGB8888:
      l->pf = PIPE_FORMAT_B8G8R8X8_UNORM;
      l->drm_fourcc = DRM_FORMAT_XRGB8888;
      l->internalformat = 0x8051;   /* GL_RGB8 */
      break;
   case SYMGL_SGFMT_RGB565:
      l->pf = PIPE_FORMAT_B5G6R5_UNORM;
      l->drm_fourcc = DRM_FORMAT_RGB565;
      l->internalformat = 0x8D62;   /* GL_RGB565 */
      break;
   default:
      return false;
   }
   /* import only the image's pages (the Mali's import window is 64 MB for all chunks together) */
   if (l->sg.stride & 7)        /* lima wants a pitch that is a multiple of 8 */
      return false;
   const uintptr_t base = (uintptr_t)l->sg.base;
   l->res_off = (unsigned)(base & 0xFFFu);
   l->imp_addr = (void *)(base - l->res_off);
   l->imp_size = (l->res_off + l->sg.stride * l->sg.height + 0xFFFu) & ~0xFFFu;
   return true;
}

/* import the image's pages and wrap them as a linear 2D resource; *prime_out = the import */
static struct pipe_resource *
sym_sg_resource(struct sym_egl_display *dpy, const struct sym_sg_layout *l, unsigned bind, int *prime_out)
{
   const int prime = sym_drm_import_addr(dpy->fd, l->imp_addr, l->imp_size);
   if (prime < 0)
      return NULL;
   struct pipe_screen *screen = dpy->disp->fscreen->screen;
   struct pipe_resource templat;
   memset(&templat, 0, sizeof(templat));
   templat.target = PIPE_TEXTURE_2D;
   templat.format = l->pf;
   templat.width0 = l->sg.width;
   templat.height0 = l->sg.height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.bind = bind | PIPE_BIND_LINEAR;
   struct winsys_handle wh;
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_FD;
   wh.handle = prime;
   wh.offset = l->res_off;
   wh.stride = l->sg.stride;
   wh.modifier = DRM_FORMAT_MOD_LINEAR;
   wh.format = l->drm_fourcc;
   struct pipe_resource *res = screen->resource_from_handle(screen, &templat, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   if (!res) {
      sym_drm_unimport(dpy->fd, prime);
      return NULL;
   }
   *prime_out = prime;
   return res;
}

static _EGLImage *
sym_create_sgimage_image(_EGLDisplay *disp, EGLClientBuffer rsgimage, const EGLint *attr_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   _EGLImageAttribs a;
   if (!_eglParseImageAttribList(&a, disp, attr_list))
      return NULL;
   struct sym_sg_layout l;
   const bool mem = attr_list == sym_mem_attr_tag;
   if (!rsgimage || !(mem ? sym_mem_layout_get((const symgl_sgimage *)rsgimage, &l) : sym_sg_layout_get(rsgimage, &l))) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateImageKHR: not an RSgImage in system memory (or a format we cannot use)");
      return NULL;
   }
   int prime = -1;
   struct pipe_resource *res = sym_sg_resource(dpy, &l, PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET, &prime);
   if (!res && mem)   /* lima renders only into a 16-pixel aligned stride: the image can still be sampled */
      res = sym_sg_resource(dpy, &l, PIPE_BIND_SAMPLER_VIEW, &prime);
   if (!res) {
      _eglError(EGL_BAD_ALLOC, "eglCreateImageKHR: the RSgImage cannot be imported");
      return NULL;
   }
   struct sym_egl_image *img = calloc(1, sizeof(*img));
   if (!img) {
      pipe_resource_reference(&res, NULL);
      sym_drm_unimport(dpy->fd, prime);
      _eglError(EGL_BAD_ALLOC, "eglCreateImageKHR");
      return NULL;
   }
   _eglInitImage(&img->base, disp);
   img->tex = res;
   img->format = l.pf;
   img->internalformat = l.internalformat;
   img->fd = dpy->fd;
   img->prime_fd = prime;
   img->range = sym_range_add(prime, l.res_off, l.sg.stride * l.sg.height);
   /* the CPU filled it: its dirty lines go to memory before the GPU samples it */
   sym_drm_cache_sync(dpy->fd, prime, l.res_off, l.sg.stride * l.sg.height, 1);
   return &img->base;
}

static _EGLSurface *
sym_create_sgimage_surface(_EGLDisplay *disp, _EGLConfig *conf, void *rsgimage, const EGLint *attrib_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   struct sym_sg_layout l;
   if (!sym_sg_layout_get(rsgimage, &l)) {
      _eglError(EGL_BAD_NATIVE_PIXMAP, "eglCreatePixmapSurface: not an RSgImage in system memory");
      return NULL;
   }
   /* the config must describe the image's format (EGL 1.4 3.5: EGL_BAD_MATCH) */
   const bool conf565 = conf->RedSize == 5, img565 = l.sg.format == SYMGL_SGFMT_RGB565;
   const bool confa = conf->AlphaSize > 0, imga = l.sg.format == SYMGL_SGFMT_ARGB8888;
   if (conf565 != img565 || (!img565 && confa != imga)) {
      _eglError(EGL_BAD_MATCH, "eglCreatePixmapSurface: the config does not match the RSgImage's pixel format");
      return NULL;
   }
   struct sym_egl_surface *surf = calloc(1, sizeof(*surf));
   if (!surf)
      return NULL;
   surf->range = -1;
   if (!_eglInitSurface(&surf->base, disp, EGL_PIXMAP_BIT, conf, attrib_list, rsgimage)) {
      free(surf);
      return NULL;
   }
   struct st_visual visual = visual_for(conf);
   visual.color_format = l.pf;
   visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK |
                        (visual.depth_stencil_format != PIPE_FORMAT_NONE ? ST_ATTACHMENT_DEPTH_STENCIL_MASK : 0);
   surf->fb = sym_create_st_framebuffer(dpy->disp, &visual, NULL, dpy->fd);
   if (!surf->fb) {
      free(surf);
      return NULL;
   }
   struct pipe_resource *res;
   if (dpy->fd >= 0) {
      int prime = -1;
      res = sym_sg_resource(dpy, &l, PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW, &prime);
      surf->fb->prime_fd = prime;
      if (res) {
         surf->range = sym_range_add(prime, l.res_off, l.sg.stride * l.sg.height);
         /* the GPU will write it: the CPU's dirty lines must not land on top later */
         sym_drm_cache_sync(dpy->fd, prime, l.res_off, l.sg.stride * l.sg.height, 1);
      }
   } else {
      /* softpipe: a display target over the chunk's CPU address */
      struct pipe_screen *screen = dpy->disp->fscreen->screen;
      struct pipe_resource templat;
      memset(&templat, 0, sizeof(templat));
      templat.target = PIPE_TEXTURE_2D;
      templat.format = l.pf;
      templat.width0 = l.sg.width;
      templat.height0 = l.sg.height;
      templat.depth0 = 1;
      templat.array_size = 1;
      templat.bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR;
      surf->fb->dt_mem[0].data = l.sg.base;
      surf->fb->dt_mem[0].stride = l.sg.stride;
      res = screen->resource_create_front(screen, &templat, &surf->fb->dt_mem[0]);
   }
   if (!res) {
      sym_range_remove(surf->range);
      sym_destroy_st_framebuffer(surf->fb);
      free(surf);
      _eglError(EGL_BAD_ALLOC, "eglCreatePixmapSurface: the RSgImage cannot be imported");
      return NULL;
   }
   surf->fb->pixmap_res = res;
   surf->base.Width = l.sg.width;
   surf->base.Height = l.sg.height;
   surf->fb->new_width = l.sg.width;
   surf->fb->new_height = l.sg.height;
   p_atomic_inc(&surf->fb->base.stamp);
   return &surf->base;
}

/* finish the current context's GPU work (glFinish semantics) */
static void
sym_finish_context(struct sym_egl_display *dpy, struct st_context *st)
{
   struct pipe_screen *screen = dpy->disp->fscreen->screen;
   struct pipe_fence_handle *fence = NULL;
   _mesa_glthread_finish(st->ctx);
   st_context_flush(st, ST_FLUSH_FRONT, &fence, NULL, NULL);
   if (fence) {
      screen->fence_finish(screen, NULL, fence, OS_TIMEOUT_INFINITE);
      screen->fence_reference(screen, &fence, NULL);
   }
}

static EGLBoolean
sym_wait_client(_EGLDisplay *disp, _EGLContext *c)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   struct sym_egl_context *ctx = sym_egl_context(c);
   if (!ctx || !ctx->st)
      return EGL_TRUE;
   sym_finish_context(dpy, ctx->st);
   if (dpy->fd >= 0)
      sym_ranges_sync(dpy->fd, 2);   /* the GPU wrote them, the CPU will read */
   return EGL_TRUE;
}

static EGLBoolean
sym_wait_native(EGLint engine)
{
   (void)engine;
   struct sym_egl_display *dpy = sym_the_disp ? sym_egl_display(sym_the_disp) : NULL;
   if (dpy && dpy->fd >= 0)
      sym_ranges_sync(dpy->fd, 1);   /* the CPU wrote them, the GPU will read/write */
   return EGL_TRUE;
}

static EGLBoolean
sym_initialize(_EGLDisplay *disp)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   if (dpy) {
      dpy->ref_count++;
      return EGL_TRUE;
   }
   dpy = calloc(1, sizeof(*dpy));
   if (!dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");
   struct pipe_screen *screen = sym_create_screen(&dpy->fd);
   if (!screen) {
      free(dpy);
      return _eglError(EGL_NOT_INITIALIZED, "eglInitialize: no pipe screen");
   }
   dpy->disp = sym_create_display(screen);
   dpy->ref_count = 1;
   disp->DriverData = dpy;

   disp->ClientAPIs = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
   disp->Extensions.KHR_no_config_context = EGL_TRUE;
   disp->Extensions.KHR_surfaceless_context = EGL_TRUE;
   disp->Extensions.KHR_create_context = EGL_TRUE;
   disp->Extensions.MESA_query_driver = EGL_TRUE;
   disp->Extensions.KHR_fence_sync = EGL_TRUE;
   disp->Extensions.KHR_reusable_sync = EGL_TRUE;
   disp->Extensions.KHR_wait_sync = EGL_TRUE;

   if (dpy->fd >= 0) {            /* G6: EGLImages over shared chunks (the Mali imports them; softpipe cannot) */
      disp->Extensions.KHR_image_base = EGL_TRUE;
      disp->Extensions.EXT_image_dma_buf_import = EGL_TRUE;
      disp->Extensions.KHR_image_pixmap = EGL_TRUE;      /* G6b: eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR, RSgImage*) */
      dpy->disp->fscreen->get_egl_image = sym_get_egl_image;
      dpy->disp->fscreen->validate_egl_image = sym_validate_egl_image;
      sym_the_disp = disp;
   }

   sym_add_config(disp, 1, 0, 0);    /* XRGB8888 + D24S8: the composition-friendly default */
   sym_add_config(disp, 2, 8, 0);    /* ARGB8888 + D24S8 */
   if (dpy->fd >= 0)
      sym_add_config(disp, 3, 0, 1); /* RGB565 + D24S8: RSgImage pixmaps only */
   return EGL_TRUE;
}

static EGLBoolean
sym_terminate(_EGLDisplay *disp)
{
   sym_display_destroy(disp);
   return EGL_TRUE;
}

static _EGLContext *
sym_egl_create_context(_EGLDisplay *disp, _EGLConfig *conf, _EGLContext *share_list, const EGLint *attrib_list)
{
   struct sym_egl_display *dpy = sym_egl_display(disp);
   struct sym_egl_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx) {
      _eglError(EGL_BAD_ALLOC, "eglCreateContext");
      return NULL;
   }
   if (!_eglInitContext(&ctx->base, disp, conf, share_list, attrib_list)) {
      symgl_log("[symgl] eglCreateContext: _eglInitContext refused the attributes/config");
      goto fail;
   }
   if (ctx->base.ClientAPI != EGL_OPENGL_ES_API) {
      symgl_log("[symgl] eglCreateContext: not EGL_OPENGL_ES_API");
      _eglError(EGL_BAD_MATCH, "eglCreateContext: OpenGL ES only");
      goto fail;
   }
   const int major = ctx->base.ClientMajorVersion;
   const gl_api api = major >= 2 ? API_OPENGLES2 : API_OPENGLES;
   struct st_visual visual = visual_for(conf);
   ctx->st = sym_create_context(dpy->disp, &visual, api, major, ctx->base.ClientMinorVersion,
                                share_list ? sym_egl_context(share_list)->st : NULL);
   if (!ctx->st) {
      _eglError(EGL_BAD_MATCH, "eglCreateContext: st_api_create_context failed");
      goto fail;
   }
   return &ctx->base;
fail:
   free(ctx);
   return NULL;
}

static EGLBoolean
sym_egl_destroy_context(_EGLDisplay *disp, _EGLContext *c)
{
   (void)disp;
   if (_eglPutContext(c)) {
      sym_destroy_context(sym_egl_context(c)->st);
      free(c);
   }
   return EGL_TRUE;
}

static EGLBoolean
sym_make_current(_EGLDisplay *disp, _EGLSurface *dsurf, _EGLSurface *rsurf, _EGLContext *c)
{
   struct sym_egl_context *ctx = sym_egl_context(c);
   struct sym_egl_surface *d = sym_egl_surface(dsurf);
   struct sym_egl_surface *r = sym_egl_surface(rsurf);
   _EGLContext *old_ctx;
   _EGLSurface *old_dsurf, *old_rsurf;

   if (!_eglBindContext(c, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
      return EGL_FALSE;
   if (old_ctx == c && old_dsurf == dsurf && old_rsurf == rsurf) {
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);
      return EGL_TRUE;
   }
   if (!c) {
      st_api_make_current(NULL, NULL, NULL);
   } else {
      if (d && dsurf != old_dsurf)
         update_size(d->fb);
      st_api_make_current(ctx->st, d ? &d->fb->base : NULL, r ? &r->fb->base : NULL);
   }
   if (old_dsurf)
      sym_destroy_surface(disp, old_dsurf);
   if (old_rsurf)
      sym_destroy_surface(disp, old_rsurf);
   if (old_ctx)
      sym_egl_destroy_context(disp, old_ctx);
   return EGL_TRUE;
}

/* ---- sync objects (EGL_KHR_fence_sync / reusable_sync / wait_sync), after dri2_create_sync & co ----------------
 * The NGA window server (eglsynchelper -> libWFC) uses reusable syncs; a fence sync wraps the pipe fence of the
 * current context's flush. */

struct sym_egl_sync {
   _EGLSync base;
   mtx_t mutex;
   cnd_t cond;
   struct pipe_fence_handle *fence;
   int refcount;
};

static struct sym_egl_sync *
sym_egl_sync(_EGLSync *s)
{
   return (struct sym_egl_sync *)s;
}

static void
sym_unref_sync(_EGLDisplay *disp, struct sym_egl_sync *sync)
{
   if (!p_atomic_dec_zero(&sync->refcount))
      return;
   if (sync->base.Type == EGL_SYNC_REUSABLE_KHR) {
      cnd_destroy(&sync->cond);
      mtx_destroy(&sync->mutex);
   }
   if (sync->fence) {
      struct pipe_screen *screen = sym_egl_display(disp)->disp->fscreen->screen;
      screen->fence_reference(screen, &sync->fence, NULL);
   }
   free(sync);
}

static _EGLSync *
sym_create_sync(_EGLDisplay *disp, EGLenum type, const EGLAttrib *attrib_list)
{
   struct sym_egl_sync *sync = calloc(1, sizeof(*sync));
   if (!sync) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSyncKHR");
      return NULL;
   }
   if (!_eglInitSync(&sync->base, disp, type, attrib_list)) {
      free(sync);
      return NULL;
   }
   switch (type) {
   case EGL_SYNC_FENCE_KHR: {
      struct sym_egl_context *ctx = sym_egl_context(_eglGetCurrentContext());
      if (!ctx) {
         _eglError(EGL_BAD_MATCH, "eglCreateSyncKHR: no current context");
         free(sync);
         return NULL;
      }
      st_context_flush(ctx->st, 0, &sync->fence, NULL, NULL);
      if (!sync->fence)   /* nothing pending: signalled at once */
         sync->base.SyncStatus = EGL_SIGNALED_KHR;
      break;
   }
   case EGL_SYNC_REUSABLE_KHR:
      if (mtx_init(&sync->mutex, mtx_plain) != thrd_success ||
          cnd_init(&sync->cond) != thrd_success) {
         _eglError(EGL_BAD_ACCESS, "eglCreateSyncKHR");
         free(sync);
         return NULL;
      }
      sync->base.SyncStatus = EGL_UNSIGNALED_KHR;
      break;
   default:
      _eglError(EGL_BAD_ATTRIBUTE, "eglCreateSyncKHR");
      free(sync);
      return NULL;
   }
   p_atomic_set(&sync->refcount, 1);
   return &sync->base;
}

static EGLBoolean
sym_destroy_sync(_EGLDisplay *disp, _EGLSync *s)
{
   struct sym_egl_sync *sync = sym_egl_sync(s);
   if (s->Type == EGL_SYNC_REUSABLE_KHR && s->SyncStatus == EGL_UNSIGNALED_KHR) {
      mtx_lock(&sync->mutex);
      s->SyncStatus = EGL_SIGNALED_KHR;    /* unblock every waiter before the object goes */
      cnd_broadcast(&sync->cond);
      mtx_unlock(&sync->mutex);
   }
   sym_unref_sync(disp, sync);
   return EGL_TRUE;
}

static EGLint
sym_client_wait_sync(_EGLDisplay *disp, _EGLSync *s, EGLint flags, EGLTime timeout)
{
   struct sym_egl_sync *sync = sym_egl_sync(s);
   struct pipe_screen *screen = sym_egl_display(disp)->disp->fscreen->screen;
   EGLint ret = EGL_CONDITION_SATISFIED_KHR;
   p_atomic_inc(&sync->refcount);

   if (flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) {
      struct sym_egl_context *ctx = sym_egl_context(_eglGetCurrentContext());
      if (ctx)
         st_context_flush(ctx->st, 0, NULL, NULL, NULL);
   }
   switch (s->Type) {
   case EGL_SYNC_FENCE_KHR:
      if (s->SyncStatus != EGL_SIGNALED_KHR) {
         if (screen->fence_finish(screen, NULL, sync->fence, timeout == EGL_FOREVER_KHR ? OS_TIMEOUT_INFINITE : timeout))
            s->SyncStatus = EGL_SIGNALED_KHR;
         else
            ret = EGL_TIMEOUT_EXPIRED_KHR;
      }
      break;
   case EGL_SYNC_REUSABLE_KHR:
      mtx_lock(&sync->mutex);
      if (s->SyncStatus != EGL_SIGNALED_KHR) {
         if (timeout == EGL_FOREVER_KHR) {
            while (s->SyncStatus != EGL_SIGNALED_KHR)
               cnd_wait(&sync->cond, &sync->mutex);
         } else {
            const int64_t deadline = os_time_get_nano() + (int64_t)timeout;
            while (s->SyncStatus != EGL_SIGNALED_KHR) {
               const int64_t now = os_time_get_nano();
               if (now >= deadline) {
                  ret = EGL_TIMEOUT_EXPIRED_KHR;
                  break;
               }
               const int64_t left = deadline - now;
               struct timespec ts;
               timespec_get(&ts, TIME_UTC);
               ts.tv_sec += left / 1000000000LL;
               ts.tv_nsec += left % 1000000000LL;
               if (ts.tv_nsec >= 1000000000L) {
                  ts.tv_sec++;
                  ts.tv_nsec -= 1000000000L;
               }
               cnd_timedwait(&sync->cond, &sync->mutex, &ts);
            }
         }
      }
      mtx_unlock(&sync->mutex);
      break;
   default:
      ret = EGL_FALSE;
      break;
   }
   sym_unref_sync(disp, sync);
   return ret;
}

static EGLBoolean
sym_signal_sync(_EGLDisplay *disp, _EGLSync *s, EGLenum mode)
{
   (void)disp;
   struct sym_egl_sync *sync = sym_egl_sync(s);
   if (s->Type != EGL_SYNC_REUSABLE_KHR)
      return _eglError(EGL_BAD_MATCH, "eglSignalSyncKHR");
   if (mode != EGL_SIGNALED_KHR && mode != EGL_UNSIGNALED_KHR)
      return _eglError(EGL_BAD_ATTRIBUTE, "eglSignalSyncKHR");
   mtx_lock(&sync->mutex);
   s->SyncStatus = mode;
   if (mode == EGL_SIGNALED_KHR)
      cnd_broadcast(&sync->cond);
   mtx_unlock(&sync->mutex);
   return EGL_TRUE;
}

static EGLint
sym_server_wait_sync(_EGLDisplay *disp, _EGLSync *s)
{
   /* one GPU queue per process: a server-side wait is a client wait */
   sym_client_wait_sync(disp, s, 0, EGL_FOREVER_KHR);
   return EGL_TRUE;
}

const _EGLDriver _eglDriver = {
   .Initialize = sym_initialize,
   .Terminate = sym_terminate,
   .CreateContext = sym_egl_create_context,
   .DestroyContext = sym_egl_destroy_context,
   .MakeCurrent = sym_make_current,
   .CreateWindowSurface = sym_create_window_surface,
   .CreatePixmapSurface = sym_create_pixmap_surface,
   .CreatePbufferSurface = sym_create_pbuffer_surface,
   .DestroySurface = sym_destroy_surface,
   .SwapBuffers = sym_swap_buffers,
   .CreateSyncKHR = sym_create_sync,
   .DestroySyncKHR = sym_destroy_sync,
   .ClientWaitSyncKHR = sym_client_wait_sync,
   .SignalSyncKHR = sym_signal_sync,
   .WaitSyncKHR = sym_server_wait_sync,
   .CreateImageKHR = sym_create_image,
   .DestroyImageKHR = sym_destroy_image,
   .WaitClient = sym_wait_client,
   .WaitNative = sym_wait_native,
};
