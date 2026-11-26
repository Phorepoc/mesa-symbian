/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* drm_symbian_vmw.c -- OURS (S3): the vmwgfx DRM uAPI emulated over the SVGA3D kernel driver (symvmw.h), so Mesa's svga driver and its DRM winsys
 * (src/gallium/winsys/svga/drm, unmodified but for the OS calls in build_mesa_symbian.py) run on the host GPU of a virtual machine that has the VMware SVGA3D device.
 *
 * What the vmwgfx kernel driver keeps (Linux), kept here in the process, the way drm_symbian.c does for lima:
 *   buffer objects (DRM_VMW_ALLOC_DMABUF)  -> ranges of the channel's POOL (a shared chunk; pages committed by the kernel driver); a buffer is a MOB:
 *                                             the handle IS the MOB id, so the winsys' "relocations" (it writes the buffer handle where the command
 *                                             wants a MOB id) need no patching;
 *   surfaces (GB_SURFACE_CREATE)           -> DEFINE_GB_SURFACE + BIND_GB_SURFACE to a backup buffer; the handle IS the surface id;
 *   DX contexts (CREATE_EXTENDED_CONTEXT)  -> DX_DEFINE_CONTEXT + the context MOB + its 12 COTables (sized for 2048 entries each; vmwgfx grows them on
 *                                             demand); the handle IS the context id;
 *   EXECBUF                                -> the winsys' command stream copied into the arena and run as one command buffer (DX context flag);
 *   fences                                 -> a sequence number; the kernel driver runs a command buffer to completion before it returns, so every fence
 *                                             is signalled at once.
 * Not emulated (errno ENOSYS): legacy (non-guest-backed) surfaces and contexts, shaders (DX shaders live in COTables), surface sharing by
 * handle (DRI), the host message channel and mksStats. fds are fake (FD_VMW). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "symgl_native.h"
#include "sym_winsys.h"
#include "symvmw.h"
#include "svga/drm/vmwgfx_drm.h"
#include "svga3d_reg.h"
#include "svga3d_devcaps.h"
#include "vmw_surf_defs.h"
#include "svga/drm/vmw_surface.h"
#include "util/simple_mtx.h"

#define FD_VMW 0x5B000000
#define PAGE 4096u
#define COTABLE_ENTRIES 2048u
#define CTX_MOB_BYTES (128u * 1024u)
#define MAX_RANGES 4096

struct bo {
   uint32_t off, size;       /* pool offset (page aligned), bytes asked for (0 = free handle) */
   int refs;                 /* the user's handle + each surface bound to it */
   void *cpu;                /* an import (sym_vmw_import_addr): the memory of this process the MOB is over; NULL = a pool buffer */
};

struct surf {
   int used;
   uint32_t bo;              /* the backup buffer handle, 0 = none */
   int imported;             /* made by sym_vmw_import_addr: what GB_SURFACE_REF answers */
   int shown;                /* an import that is the screen target (sym_vmw_present_import): never read back */
   int target;               /* an import the GPU draws into (READBACK after each command stream), else a source the CPU writes (UPDATE before) */
   uint32_t format, width, height, flags, backup;
};

/* mmap offset of an import's buffer (outside the pool's offsets): the import's own address is returned */
#define IMPORT_MAP ((uint64_t)1 << 40)
/* "prime fds" of imported surfaces: PRIME_BASE + surface id */
#define PRIME_BASE 0x40000000

struct ctx {
   int used, dx;
   uint32_t mob[1 + SVGA_COTABLE_MAX];   /* the context MOB, then the COTables' */
};

struct range {
   uint32_t first, pages;
};

static struct {
   symvmw_t *v;
   symvmw_info_t info;
   int refs;
   uint32_t devcaps[SYMVMW_DEVCAPS];
   uint8_t *arena;
   uint32_t arena_bytes;
   struct bo *bos;
   struct surf *surfs;
   struct ctx *ctxs;
   struct range free_list[MAX_RANGES];
   int nfree;
   uint32_t seqno;
   uint32_t surf_high, ctx_high;
   uint32_t submits, errors, creates;
   int import_stale;         /* the CPU may have written imported sources since the last UPDATE */
} V;

static simple_mtx_t vmw_mtx = SIMPLE_MTX_INITIALIZER;

/* S5 profile: command buffers run and the microseconds spent in them (egl_symbian.c prints and clears them) */
extern unsigned sym_vmw_prof_cbs, sym_vmw_prof_us, sym_vmw_prof_bytes, sym_vmw_prof_ioctls, sym_vmw_prof_ioctl_us, sym_vmw_prof_objs, sym_vmw_prof_obj_us;

static void
vlog(const char *fmt, ...)
{
   char buf[176];
   va_list ap;
   va_start(ap, fmt);
   buf[0] = 'P'; buf[1] = 'H'; buf[2] = ':'; buf[3] = ' ';
   vsnprintf(buf + 4, sizeof(buf) - 4, fmt, ap);
   va_end(ap);
   symgl_log(buf);
}

/* a slow step of an object create / destroy (the first 40 over 2 ms), with where the time went: a = pool commit, b = the MOB define / destroy */
static void
slow_log(const char *what, uint32_t size, unsigned a_us, unsigned b_us)
{
   static int n;
   if (a_us + b_us > 2000 && n++ < 40)
      vlog("svga: slow %s: %u bytes, commit %u us, mob %u us", what, size, a_us, b_us);
}

/* ---- the pool's address space: first fit over a sorted list of free page ranges ---------------------------------------------- */

static int
pool_alloc(uint32_t pages, uint32_t *first)
{
   for (int i = 0; i < V.nfree; i++) {
      if (V.free_list[i].pages >= pages) {
         *first = V.free_list[i].first;
         V.free_list[i].first += pages;
         V.free_list[i].pages -= pages;
         if (!V.free_list[i].pages) {
            memmove(&V.free_list[i], &V.free_list[i + 1], (V.nfree - i - 1) * sizeof(struct range));
            V.nfree--;
         }
         return 0;
      }
   }
   return -1;
}

static void
pool_free(uint32_t first, uint32_t pages)
{
   int i = 0;
   while (i < V.nfree && V.free_list[i].first < first)
      i++;
   if (i > 0 && V.free_list[i - 1].first + V.free_list[i - 1].pages == first) {
      V.free_list[i - 1].pages += pages;
      i--;
   } else if (V.nfree < MAX_RANGES) {
      memmove(&V.free_list[i + 1], &V.free_list[i], (V.nfree - i) * sizeof(struct range));
      V.free_list[i].first = first;
      V.free_list[i].pages = pages;
      V.nfree++;
   } else {
      return;   /* (the list is full of holes: this range stays lost) */
   }
   if (i + 1 < V.nfree && V.free_list[i].first + V.free_list[i].pages == V.free_list[i + 1].first) {
      V.free_list[i].pages += V.free_list[i + 1].pages;
      memmove(&V.free_list[i + 1], &V.free_list[i + 2], (V.nfree - i - 2) * sizeof(struct range));
      V.nfree--;
   }
}

/* ---- command buffers ---------------------------------------------------------------------------------------------------------- */

struct cmdw {
   uint32_t *p;
   uint32_t bytes;
};

static void
cmd_begin(struct cmdw *c)
{
   c->p = (uint32_t *)V.arena;
   c->bytes = 0;
}

static void *
cmd_add(struct cmdw *c, uint32_t id, uint32_t body_bytes)
{
   uint32_t *h = (uint32_t *)((uint8_t *)c->p + c->bytes);
   h[0] = id;
   h[1] = body_bytes;
   c->bytes += 8 + body_bytes;
   return h + 2;
}

/* run the staged commands; DX context cid (or SVGA3D_INVALID_ID for none). 0 or -errno */
static int
cmd_run(const struct cmdw *c, uint32_t cid, const char *what)
{
   uint32_t status = 0, err_off = 0;
   V.submits++;
   const unsigned t0 = symgl_now_us();
   int r = symvmw_submit(V.v, 0, c->bytes, 0, cid != SVGA3D_INVALID_ID ? 2 : 0, cid != SVGA3D_INVALID_ID ? cid : 0,
                         V.surf_high, V.ctx_high, &status, &err_off);
   sym_vmw_prof_us += SYM_DUS(t0, symgl_now_us());
   sym_vmw_prof_cbs++;
   sym_vmw_prof_bytes += c->bytes;
   if (r != 0 || status != 1) {
      if (V.errors++ < 24)
         vlog("svga: %s: submit r=%d status %u error offset %u (%u bytes, context %u)", what, r, status, err_off, c->bytes,
              cid);
      return r != 0 ? -EIO : -EINVAL;
   }
   return 0;
}

/* the zero-copy import's refusals (egl_symbian.c too): the first 32, whatever else went wrong before */
void
sym_vmw_import_log(const char *fmt, ...)
{
   static int n;
   if (n++ >= 32)
      return;
   char buf[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   vlog("import: %s", buf);
}

/* ---- buffer objects ----------------------------------------------------------------------------------------------------------- */

static struct bo *
bo_of(uint32_t handle)
{
   if (handle < V.info.mob_base || handle >= V.info.mob_base + V.info.mob_count)
      return NULL;
   struct bo *b = &V.bos[handle - V.info.mob_base];
   return b->size ? b : NULL;
}

/* a buffer: pool pages committed by the kernel driver and defined as a MOB (id = handle) */
static int
bo_create(uint32_t size, uint32_t *handle)
{
   if (!size)
      return -EINVAL;
   const uint32_t pages = (size + PAGE - 1) / PAGE;
   uint32_t first = 0, h = 0;
   for (uint32_t i = 0; i < V.info.mob_count; i++)
      if (!V.bos[i].size) {
         h = V.info.mob_base + i;
         break;
      }
   if (!h) {
      vlog("svga: out of MOB ids");
      return -ENOMEM;
   }
   if (pool_alloc(pages, &first) != 0)
      return -ENOMEM;
   const unsigned tc0 = symgl_now_us();
   int r = symvmw_pool_commit(V.v, first * PAGE, pages * PAGE);
   const unsigned tc1 = symgl_now_us();
   if (r == 0)
      r = symvmw_mob_define(V.v, h, first * PAGE, size);
   {
      static int nlog;
      if (SYM_DUS(tc0, tc1) > 2000 && nlog++ < 24)
         vlog("svga: slow bo_create: %u bytes at page %u (%u pages), %d free ranges, %u buffers made so far", size, first, pages, V.nfree, V.creates);
   }
   slow_log("bo_create", size, SYM_DUS(tc0, tc1), SYM_DUS(tc1, symgl_now_us()));
   if (r != 0) {
      if (V.errors++ < 24)
         vlog("svga: buffer of %u bytes: commit / define MOB %u failed (%d)", size, h, r);
      pool_free(first, pages);
      return r == -4 /* KErrNoMemory */ ? -ENOMEM : -EIO;
   }
   struct bo *b = &V.bos[h - V.info.mob_base];
   b->off = first * PAGE;
   b->size = size;
   b->refs = 1;
   *handle = h;
   V.creates++;
   return 0;
}

static void
bo_unref(uint32_t handle)
{
   struct bo *b = bo_of(handle);
   if (!b || --b->refs > 0)
      return;
   const unsigned tu0 = symgl_now_us();
   symvmw_mob_destroy(V.v, handle);   /* (an import: the kernel driver unpins the pages and lets go of the chunk) */
   slow_log("bo_unref", b->size, 0, SYM_DUS(tu0, symgl_now_us()));
   if (b->cpu) {
      memset(b, 0, sizeof(*b));
      return;
   }
   pool_free(b->off / PAGE, (b->size + PAGE - 1) / PAGE);   /* (the pages stay committed: the next buffer here reuses them) */
   memset(b, 0, sizeof(*b));
}

/* ---- the ioctls --------------------------------------------------------------------------------------------------------------- */

static int
do_getparam(struct drm_vmw_getparam_arg *a)
{
   uint64_t v = 0;
   switch (a->param) {
   case DRM_VMW_PARAM_3D: v = V.info.devcap_3d; break;
   case DRM_VMW_PARAM_HW_CAPS: v = V.info.caps; break;
   case DRM_VMW_PARAM_FIFO_CAPS: v = 0; break;
   case DRM_VMW_PARAM_MAX_FB_SIZE: v = 128u << 20; break;
   case DRM_VMW_PARAM_FIFO_HW_VERSION: v = SVGA3D_HWVERSION_CURRENT; break;
   case DRM_VMW_PARAM_MAX_SURF_MEMORY: v = V.info.pool_bytes; break;
   case DRM_VMW_PARAM_3D_CAPS_SIZE: v = SYMVMW_DEVCAPS * 4; break;
   case DRM_VMW_PARAM_MAX_MOB_MEMORY: v = V.info.mob_memory; break;
   case DRM_VMW_PARAM_MAX_MOB_SIZE: v = V.info.mob_memory; break;
   case DRM_VMW_PARAM_SCREEN_TARGET: v = 0; break;
   case DRM_VMW_PARAM_DX: v = V.info.devcap_dx; break;
   case DRM_VMW_PARAM_HW_CAPS2: v = V.info.cap2; break;
   case DRM_VMW_PARAM_SM4_1: v = V.info.devcap_sm41; break;
   case DRM_VMW_PARAM_SM5: v = V.info.devcap_sm5; break;
   case DRM_VMW_PARAM_GL43: v = V.info.devcap_gl43; break;
   case DRM_VMW_PARAM_DEVICE_ID: v = 0x0405; break;
   case DRM_VMW_PARAM_USER_SRF: v = 0; break;
   default: return -EINVAL;
   }
   a->value = v;
   return 0;
}

static int
do_create_context(uint32_t *cid_out)
{
   uint32_t cid = 0;
   for (uint32_t i = 0; i < V.info.ctx_count; i++)
      if (!V.ctxs[i].used) {
         cid = V.info.ctx_base + i;
         break;
      }
   if (!cid)
      return -ENOMEM;
   struct ctx *c = &V.ctxs[cid - V.info.ctx_base];
   memset(c, 0, sizeof(*c));
   /* the context's state MOB and its COTables: vmw_dx_context_create + vmw_cotable_alloc */
   static const uint32_t entry_bytes[SVGA_COTABLE_MAX] = {
      sizeof(SVGACOTableDXRTViewEntry), sizeof(SVGACOTableDXDSViewEntry), sizeof(SVGACOTableDXSRViewEntry),
      sizeof(SVGACOTableDXElementLayoutEntry), sizeof(SVGACOTableDXBlendStateEntry), sizeof(SVGACOTableDXDepthStencilEntry),
      sizeof(SVGACOTableDXRasterizerStateEntry), sizeof(SVGACOTableDXSamplerEntry), sizeof(SVGACOTableDXStreamOutputEntry),
      sizeof(SVGACOTableDXQueryEntry), sizeof(SVGACOTableDXShaderEntry), sizeof(SVGACOTableDXUAViewEntry)};
   int r = bo_create(CTX_MOB_BYTES, &c->mob[0]);
   for (int i = 0; i < SVGA_COTABLE_MAX && r == 0; i++)
      r = bo_create(entry_bytes[i] * COTABLE_ENTRIES, &c->mob[1 + i]);
   if (r != 0) {
      for (int i = 0; i <= SVGA_COTABLE_MAX; i++)
         if (c->mob[i])
            bo_unref(c->mob[i]);
      memset(c, 0, sizeof(*c));
      return r;
   }
   struct cmdw w;
   cmd_begin(&w);
   *(SVGA3dCmdDXDefineContext *)cmd_add(&w, SVGA_3D_CMD_DX_DEFINE_CONTEXT, sizeof(SVGA3dCmdDXDefineContext)) =
      (SVGA3dCmdDXDefineContext){cid};
   *(SVGA3dCmdDXBindContext *)cmd_add(&w, SVGA_3D_CMD_DX_BIND_CONTEXT, sizeof(SVGA3dCmdDXBindContext)) =
      (SVGA3dCmdDXBindContext){cid, c->mob[0], 0};
   for (int i = 0; i < SVGA_COTABLE_MAX; i++)
      *(SVGA3dCmdDXSetCOTable *)cmd_add(&w, SVGA_3D_CMD_DX_SET_COTABLE, sizeof(SVGA3dCmdDXSetCOTable)) =
         (SVGA3dCmdDXSetCOTable){cid, c->mob[1 + i], (SVGACOTableType)i, 0};
   r = cmd_run(&w, SVGA3D_INVALID_ID, "DX context");
   if (r != 0) {
      for (int i = 0; i <= SVGA_COTABLE_MAX; i++)
         bo_unref(c->mob[i]);
      memset(c, 0, sizeof(*c));
      return r;
   }
   c->used = c->dx = 1;
   if (cid - V.info.ctx_base + 1 > V.ctx_high)
      V.ctx_high = cid - V.info.ctx_base + 1;
   *cid_out = cid;
   return 0;
}

static int
do_unref_context(uint32_t cid)
{
   if (cid < V.info.ctx_base || cid >= V.info.ctx_base + V.info.ctx_count || !V.ctxs[cid - V.info.ctx_base].used)
      return -EINVAL;
   struct ctx *c = &V.ctxs[cid - V.info.ctx_base];
   struct cmdw w;
   cmd_begin(&w);
   *(SVGA3dCmdDXDestroyContext *)cmd_add(&w, SVGA_3D_CMD_DX_DESTROY_CONTEXT, sizeof(SVGA3dCmdDXDestroyContext)) =
      (SVGA3dCmdDXDestroyContext){cid};
   cmd_run(&w, SVGA3D_INVALID_ID, "DX context destroy");
   for (int i = 0; i <= SVGA_COTABLE_MAX; i++)
      if (c->mob[i])
         bo_unref(c->mob[i]);
   memset(c, 0, sizeof(*c));
   return 0;
}

static int
do_gb_surface_create(const struct drm_vmw_gb_surface_create_req *rq, uint32_t flags_upper, uint32_t pattern, uint32_t quality,
                     uint32_t byte_stride, struct drm_vmw_gb_surface_create_rep *rep)
{
   uint32_t sid = 0;
   for (uint32_t i = 0; i < V.info.surf_count; i++)
      if (!V.surfs[i].used) {
         sid = V.info.surf_base + i;
         break;
      }
   if (!sid)
      return -ENOMEM;
   uint64_t flags = ((uint64_t)flags_upper << 32) | rq->svga3d_flags;
   if (rq->drm_surface_flags & drm_vmw_surface_flag_scanout)
      flags |= SVGA3D_SURFACE_SCREENTARGET;   /* (vmwgfx does the same: a scan-out surface can be bound to a screen target) */
   const SVGA3dSize size = {rq->base_size.width, rq->base_size.height, rq->base_size.depth};
   const uint32_t layers = rq->array_size ? rq->array_size : (rq->svga3d_flags & SVGA3D_SURFACE_CUBEMAP) ? 6 : 1;
   const uint32_t samples = rq->multisample_count ? rq->multisample_count : 1;
   const uint32_t backup = vmw_surf_get_serialized_size_extended((SVGA3dSurfaceFormat)rq->format, size, rq->mip_levels, layers, samples);
   if (!backup || backup == MAX_UINT32)
      return -EINVAL;
   uint32_t bo = 0;
   int r = 0, created = 0;
   if (rq->buffer_handle != SVGA3D_INVALID_ID && rq->buffer_handle) {
      struct bo *b = bo_of(rq->buffer_handle);
      if (!b || b->size < backup)
         return -EINVAL;
      bo = rq->buffer_handle;
   } else if (rq->drm_surface_flags & drm_vmw_surface_flag_create_buffer) {
      r = bo_create(backup, &bo);
      if (r != 0)
         return r;
      created = 1;
   }
   struct cmdw w;
   cmd_begin(&w);
   SVGA3dCmdDefineGBSurface_v4 *d = cmd_add(&w, SVGA_3D_CMD_DEFINE_GB_SURFACE_V4, sizeof(*d));
   memset(d, 0, sizeof(*d));
   d->sid = sid;
   d->surfaceFlags = flags;
   d->format = (SVGA3dSurfaceFormat)rq->format;
   d->numMipLevels = rq->mip_levels;
   d->multisampleCount = rq->multisample_count;
   d->multisamplePattern = (SVGA3dMSPattern)pattern;
   d->qualityLevel = (SVGA3dMSQualityLevel)quality;
   d->autogenFilter = (SVGA3dTextureFilter)rq->autogen_filter;
   d->size = size;
   d->arraySize = rq->array_size;
   d->bufferByteStride = byte_stride;
   if (bo) {
      SVGA3dCmdBindGBSurface *bind = cmd_add(&w, SVGA_3D_CMD_BIND_GB_SURFACE, sizeof(*bind));
      bind->sid = sid;
      bind->mobid = bo;
   }
   struct surf *s = &V.surfs[sid - V.info.surf_base];
   if (sid - V.info.surf_base + 1 > V.surf_high)
      V.surf_high = sid - V.info.surf_base + 1;
   r = cmd_run(&w, SVGA3D_INVALID_ID, "surface");
   if (r != 0) {
      if (created)
         bo_unref(bo);
      return r;
   }
   s->used = 1;
   s->bo = bo;
   if (bo)
      bo_of(bo)->refs++;   /* the surface's binding (the handle the create made, or the caller's, is the user's own reference) */
   rep->handle = sid;
   rep->backup_size = backup;
   rep->buffer_handle = bo ? bo : SVGA3D_INVALID_ID;
   rep->buffer_size = bo ? backup : 0;
   rep->buffer_map_handle = bo ? bo_of(bo)->off : 0;
   return 0;
}

static int
do_unref_surface(uint32_t sid)
{
   if (sid < V.info.surf_base || sid >= V.info.surf_base + V.info.surf_count || !V.surfs[sid - V.info.surf_base].used)
      return -EINVAL;
   struct surf *s = &V.surfs[sid - V.info.surf_base];
   struct cmdw w;
   cmd_begin(&w);
   SVGA3dCmdDestroyGBSurface *d = cmd_add(&w, SVGA_3D_CMD_DESTROY_GB_SURFACE, sizeof(*d));
   d->sid = sid;
   cmd_run(&w, SVGA3D_INVALID_ID, "surface destroy");
   if (s->bo)
      bo_unref(s->bo);   /* the surface's binding; the user's handle goes with UNREF_DMABUF */
   memset(s, 0, sizeof(*s));
   return 0;
}

static int
do_execbuf(const struct drm_vmw_execbuf_arg *a)
{
   if (a->command_size > V.arena_bytes) {
      vlog("svga: command stream of %u bytes does not fit the %u byte arena", a->command_size, V.arena_bytes);
      return -E2BIG;
   }
   struct drm_vmw_fence_rep *rep = (struct drm_vmw_fence_rep *)(uintptr_t)a->fence_rep;
   uint32_t seq = ++V.seqno;
   int targets = 0;
   /* OURS (round 43 test): where an execbuf's time goes -- the import UPDATEs (the host re-reads every imported source,
      whole), the stream itself, the target READBACKs; every 60 calls */
   static unsigned r43_n, r43_up, r43_upn, r43_ex, r43_rb, r43_rbn, r43_max;
   const unsigned r43_t0 = symgl_now_us();
   unsigned r43_t1 = r43_t0, r43_t2 = r43_t0;
   if (a->command_size && V.import_stale) {   /* zero-copy imports: the host re-reads what the CPU wrote into the sources */
      struct cmdw w;
      cmd_begin(&w);
      for (uint32_t i = 0; i < V.surf_high && w.bytes + 16 <= V.arena_bytes; i++)
         if (V.surfs[i].imported && !V.surfs[i].target) {
            SVGA3dCmdUpdateGBSurface *c = cmd_add(&w, SVGA_3D_CMD_UPDATE_GB_SURFACE, sizeof(*c));
            c->sid = V.info.surf_base + i;
         }
      if (w.bytes) {
         cmd_run(&w, SVGA3D_INVALID_ID, "import update");
         r43_upn += w.bytes / 16;
      }
      V.import_stale = 0;
   }
   r43_t1 = symgl_now_us();
   if (a->command_size) {
      struct cmdw w;
      cmd_begin(&w);
      memcpy(V.arena, (const void *)(uintptr_t)a->commands, a->command_size);
      w.bytes = a->command_size;
      const uint32_t cid = a->version > 1 ? a->context_handle : SVGA3D_INVALID_ID;
      cmd_run(&w, cid, "execbuf");   /* (an error in the stream is the stream's: vmwgfx does not fail the ioctl either) */
      r43_t2 = symgl_now_us();
      cmd_begin(&w);               /* zero-copy imports the GPU draws into: their guest pages get the host's pixels */
      for (uint32_t i = 0; i < V.surf_high && w.bytes + 16 <= V.arena_bytes; i++)
         if (V.surfs[i].imported && V.surfs[i].target && !V.surfs[i].shown) {
            SVGA3dCmdReadbackGBSurface *c = cmd_add(&w, SVGA_3D_CMD_READBACK_GB_SURFACE, sizeof(*c));
            c->sid = V.info.surf_base + i;
            targets++;
         }
      if (targets) {
         cmd_run(&w, SVGA3D_INVALID_ID, "import readback");
         V.import_stale = 1;   /* (a frame ended: the next one's sources are new) */
      }
   }
   {
      const unsigned t3 = symgl_now_us();
      if (r43_t1 >= r43_t0 && t3 >= r43_t1) {
         r43_up += r43_t1 - r43_t0;
         if (r43_t2 >= r43_t1) { r43_ex += r43_t2 - r43_t1; r43_rb += t3 - r43_t2; }
         if (t3 - r43_t0 > r43_max) r43_max = t3 - r43_t0;
      }
      r43_rbn += targets;
      if (++r43_n == 60) {
         vlog("[r43] execbuf x60: update %u us (%u surfaces) | stream %u us | readback %u us (%u targets) | max %u us (avg/call)",
              r43_up / 60, r43_upn, r43_ex / 60, r43_rb / 60, r43_rbn, r43_max);
         r43_n = r43_up = r43_upn = r43_ex = r43_rb = r43_rbn = r43_max = 0;
      }
   }
   if (rep) {
      rep->handle = seq;
      rep->mask = DRM_VMW_FENCE_FLAG_EXEC | DRM_VMW_FENCE_FLAG_QUERY;
      rep->seqno = seq;
      rep->passed_seqno = seq;
      rep->fd = -1;
      rep->error = 0;
   }
   return 0;
}

static int
dispatch(unsigned long cmd, void *data, unsigned long size)
{
   (void)size;
   switch (cmd) {
   case DRM_VMW_GET_PARAM:
      return do_getparam(data);
   case DRM_VMW_GET_3D_CAP: {
      struct drm_vmw_get_3d_cap_arg *a = data;
      memcpy((void *)(uintptr_t)a->buffer, V.devcaps, a->max_size < SYMVMW_DEVCAPS * 4 ? a->max_size : SYMVMW_DEVCAPS * 4);
      return 0;
   }
   case DRM_VMW_CREATE_EXTENDED_CONTEXT: {
      union drm_vmw_extended_context_arg *a = data;
      if (a->req != drm_vmw_context_dx)
         return -ENOSYS;
      uint32_t cid;
      int r = do_create_context(&cid);
      if (r == 0)
         a->rep.cid = cid;
      return r;
   }
   case DRM_VMW_UNREF_CONTEXT:
      return do_unref_context(((struct drm_vmw_context_arg *)data)->cid);
   case DRM_VMW_ALLOC_DMABUF: {
      union drm_vmw_alloc_dmabuf_arg *a = data;
      uint32_t h;
      int r = bo_create(a->req.size, &h);
      if (r != 0)
         return r;
      struct bo *b = bo_of(h);
      memset(&a->rep, 0, sizeof(a->rep));
      a->rep.handle = h;
      a->rep.map_handle = b->off;
      a->rep.cur_gmr_id = h;
      return 0;
   }
   case DRM_VMW_UNREF_DMABUF:
      if (!bo_of(((struct drm_vmw_handle_close_arg *)data)->handle))
         return -EINVAL;
      bo_unref(((struct drm_vmw_handle_close_arg *)data)->handle);
      return 0;
   case DRM_VMW_GB_SURFACE_CREATE: {
      union drm_vmw_gb_surface_create_arg *a = data;
      struct drm_vmw_gb_surface_create_req rq = a->req;
      return do_gb_surface_create(&rq, 0, 0, 0, 0, &a->rep);
   }
   case DRM_VMW_GB_SURFACE_CREATE_EXT: {
      union drm_vmw_gb_surface_create_ext_arg *a = data;
      struct drm_vmw_gb_surface_create_ext_req rq = a->req;
      return do_gb_surface_create(&rq.base, rq.svga3d_flags_upper_32_bits, rq.multisample_pattern, rq.quality_level,
                                  rq.buffer_byte_stride, &a->rep);
   }
   case DRM_VMW_GB_SURFACE_REF:
   case DRM_VMW_GB_SURFACE_REF_EXT: {
      uint32_t sid = ((struct drm_vmw_surface_arg *)data)->sid;
      if (((struct drm_vmw_surface_arg *)data)->handle_type == DRM_VMW_HANDLE_PRIME || sid >= PRIME_BASE)
         sid -= PRIME_BASE;   /* (DRM >= 2.6: Mesa passes the prime fd itself as the sid) */
      if (sid < V.info.surf_base || sid >= V.info.surf_base + V.info.surf_count || !V.surfs[sid - V.info.surf_base].imported)
         return -EINVAL;   /* (only imports are shared by handle) */
      const struct surf *sf = &V.surfs[sid - V.info.surf_base];
      struct drm_vmw_gb_surface_create_req *creq;
      struct drm_vmw_gb_surface_create_rep *crep;
      if (cmd == DRM_VMW_GB_SURFACE_REF_EXT) {
         union drm_vmw_gb_surface_reference_ext_arg *a = data;
         memset(&a->rep, 0, sizeof(a->rep));
         creq = &a->rep.creq.base;
         crep = &a->rep.crep;
      } else {
         union drm_vmw_gb_surface_reference_arg *a = data;
         memset(&a->rep, 0, sizeof(a->rep));
         creq = &a->rep.creq;
         crep = &a->rep.crep;
      }
      creq->svga3d_flags = sf->flags;
      creq->format = sf->format;
      creq->mip_levels = 1;
      creq->base_size.width = sf->width;
      creq->base_size.height = sf->height;
      creq->base_size.depth = 1;
      creq->buffer_handle = sf->bo;
      crep->handle = sid;
      crep->backup_size = sf->backup;
      crep->buffer_handle = sf->bo;
      crep->buffer_size = sf->backup;
      crep->buffer_map_handle = IMPORT_MAP | sf->bo;
      bo_of(sf->bo)->refs++;   /* the region Mesa makes of it is released as a buffer (UNREF_DMABUF) */
      return 0;
   }
   case DRM_VMW_UNREF_SURFACE:
      return do_unref_surface(((struct drm_vmw_surface_arg *)data)->sid);
   case DRM_VMW_EXECBUF:
      return do_execbuf(data);
   case DRM_VMW_SYNCCPU:
      return 0;   /* every command buffer has completed when its ioctl returns: the CPU may always touch a buffer */
   case DRM_VMW_FENCE_WAIT:
      return 0;
   case DRM_VMW_FENCE_SIGNALED: {
      struct drm_vmw_fence_signaled_arg *a = data;
      a->signaled = 1;
      a->passed_seqno = V.seqno;
      a->signaled_flags = a->flags;
      return 0;
   }
   case DRM_VMW_FENCE_UNREF:
      return 0;
   default:
      return -ENOSYS;
   }
}

/* ---- the libdrm surface the winsys uses --------------------------------------------------------------------------------------- */

static int
command(int fd, unsigned long cmd, void *data, unsigned long size)
{
   if (fd != FD_VMW || !V.v)
      return -EBADF;
   simple_mtx_lock(&vmw_mtx);
   const unsigned t0 = symgl_now_us();
   int r = dispatch(cmd, data, size);
   const unsigned t1 = symgl_now_us();
   const unsigned dt = SYM_DUS(t0, t1);
   simple_mtx_unlock(&vmw_mtx);
   sym_vmw_prof_ioctls++;
   sym_vmw_prof_ioctl_us += dt;
   slow_log("ioctl", (unsigned)cmd, dt, 0);
   if (cmd == DRM_VMW_ALLOC_DMABUF || cmd == DRM_VMW_GB_SURFACE_CREATE || cmd == DRM_VMW_GB_SURFACE_CREATE_EXT || cmd == DRM_VMW_UNREF_DMABUF || cmd == DRM_VMW_UNREF_SURFACE) {
      sym_vmw_prof_objs++;
      sym_vmw_prof_obj_us += dt;
   }
   return r;
}

int
drmCommandNone(int fd, unsigned long idx)
{
   return command(fd, idx, NULL, 0);
}

int
drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return command(fd, idx, data, size);
}

int
drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
   return command(fd, idx, data, size);
}

int
drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return command(fd, idx, data, size);
}

static drmVersion vmw_version = {2, 15, 0, 6, "vmwgfx", 8, "20260101", 20, "VMware SVGA3D"};

drmVersionPtr
sym_vmw_drmGetVersion(int fd)
{
   return fd == FD_VMW ? &vmw_version : NULL;
}

void
sym_vmw_drmFreeVersion(drmVersionPtr v)
{
   (void)v;
}

/* surface sharing by handle / dma-buf is DRI's (no window system here) */
int
sym_vmw_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   return -ENOSYS;
}

int
sym_vmw_drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   const uint32_t sid = (uint32_t)prime_fd - PRIME_BASE;
   if (prime_fd < PRIME_BASE || sid < V.info.surf_base || sid >= V.info.surf_base + V.info.surf_count || !V.surfs[sid - V.info.surf_base].imported)
      return -EINVAL;
   *handle = sid;
   return 0;
}

/* Zero-copy import (the vmwgfx prime import): memory of this process in a shared chunk (page aligned, rows of exactly width * 4
   bytes) becomes a MOB over the chunk's own pages and a GB surface backed by it. Returns a "prime fd" for resource_from_handle, or -1.
   svga_format: SVGA3dSurfaceFormat; flags: SVGA3D_SURFACE_* (bind flags) */
int
sym_vmw_import_addr(const void *addr, uint32_t width, uint32_t height, uint32_t svga_format, uint32_t flags)
{
   if (!V.v || ((uintptr_t)addr & (PAGE - 1)) || !width || !height) {
      sym_vmw_import_log("%p %ux%u refused (device %d, not page aligned or empty)", addr, width, height, V.v != NULL);
      return -1;
   }
   simple_mtx_lock(&vmw_mtx);
   const uint32_t size = width * 4 * height;
   uint32_t h = 0;
   for (uint32_t i = 0; i < V.info.mob_count; i++)
      if (!V.bos[i].size) {
         h = V.info.mob_base + i;
         break;
      }
   int r = h ? symvmw_mob_define_addr(V.v, h, addr, (size + PAGE - 1) & ~(PAGE - 1)) : -4;
   if (r != 0) {
      sym_vmw_import_log("%p (%ux%u): MOB %u over the chunk failed (%d)", addr, width, height, h, r);
      simple_mtx_unlock(&vmw_mtx);
      return -1;
   }
   struct bo *b = &V.bos[h - V.info.mob_base];
   b->off = 0;
   b->size = size;
   b->refs = 1;   /* the import's own (sym_vmw_unimport) */
   b->cpu = (void *)addr;
   struct drm_vmw_gb_surface_create_req rq;
   memset(&rq, 0, sizeof(rq));
   rq.svga3d_flags = flags;
   rq.format = svga_format;
   rq.mip_levels = 1;
   rq.base_size.width = width;
   rq.base_size.height = height;
   rq.base_size.depth = 1;
   rq.array_size = 1;          /* a DX (vgpu10) surface: one layer, as Mesa's own vgpu10 surfaces */
   rq.buffer_handle = h;
   struct drm_vmw_gb_surface_create_rep rep;
   memset(&rep, 0, sizeof(rep));
   r = do_gb_surface_create(&rq, 0, 0, 0, 0, &rep);
   if (r != 0 || rep.backup_size > b->size) {
      if (r == 0)
         do_unref_surface(rep.handle);
      sym_vmw_import_log("%p (%ux%u format %u flags %x): GB surface failed (%d, backup %u of %u)", addr, width, height, svga_format, flags, r,
                         rep.backup_size, b->size);
      bo_unref(h);
      simple_mtx_unlock(&vmw_mtx);
      return -1;
   }
   struct surf *sf = &V.surfs[rep.handle - V.info.surf_base];
   sf->imported = 1;
   sf->format = svga_format;
   sf->width = width;
   sf->height = height;
   sf->flags = flags;
   sf->backup = rep.backup_size;
   simple_mtx_unlock(&vmw_mtx);
   return PRIME_BASE + (int)rep.handle;
}

/* egl_symbian.c's memory images: DRM fourcc ARGB8888 / XRGB8888 (B8G8R8A8 / B8G8R8X8 in memory) */
int
sym_vmw_import_image(const void *addr, uint32_t width, uint32_t height, uint32_t drm_fourcc, int render_target, int is_target)
{
   uint32_t fmt;
   if (drm_fourcc == 0x34325241 /* AR24 */)
      fmt = SVGA3D_B8G8R8A8_UNORM;
   else if (drm_fourcc == 0x34325258 /* XR24 */)
      fmt = SVGA3D_B8G8R8X8_UNORM;
   else {
      sym_vmw_import_log("%p: fourcc %08x not importable", addr, drm_fourcc);
      return -1;
   }
   uint32_t flags = SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_BIND_SHADER_RESOURCE;
   if (render_target)
      flags |= SVGA3D_SURFACE_HINT_RENDERTARGET | SVGA3D_SURFACE_BIND_RENDER_TARGET;
   if (render_target && is_target)
      flags |= SVGA3D_SURFACE_SCREENTARGET;   /* the composer's target can be the screen target (sym_vmw_present_import), as vmwgfx's scan-out surfaces */
   const int prime = sym_vmw_import_addr(addr, width, height, fmt, flags);
   sym_vmw_import_log("%p %ux%u svga format %u rt %d target %d -> %d", addr, width, height, fmt, render_target, is_target, prime);
   if (prime >= PRIME_BASE) {
      V.surfs[prime - PRIME_BASE - V.info.surf_base].target = is_target;
      V.import_stale = 1;
   }
   return prime;
}

/* the import's own reference: the MOB goes when the surface (Mesa's texture) has gone as well */
void
sym_vmw_unimport(int prime_fd)
{
   const uint32_t sid = (uint32_t)prime_fd - PRIME_BASE;
   simple_mtx_lock(&vmw_mtx);
   if (prime_fd >= PRIME_BASE && sid >= V.info.surf_base && sid < V.info.surf_base + V.info.surf_count) {
      struct surf *sf = &V.surfs[sid - V.info.surf_base];
      if (sf->imported && sf->bo) {
         const uint32_t bo = sf->bo;
         sf->imported = 0;   /* (the surface itself goes with Mesa's UNREF_SURFACE) */
         bo_unref(bo);
      }
   }
   simple_mtx_unlock(&vmw_mtx);
}

/* keep the host's copy of an imported surface and its guest pages in step: to_device = 1 after the CPU wrote them (UPDATE: the host
   re-reads the MOB), 0 after the GPU drew into it (READBACK: the MOB gets the host's pixels; the command buffer has completed on
   return) */
int
sym_vmw_surface_sync(int prime_fd, int to_device)
{
   const uint32_t sid = (uint32_t)prime_fd - PRIME_BASE;
   if (prime_fd < PRIME_BASE)
      return -EINVAL;
   simple_mtx_lock(&vmw_mtx);
   struct cmdw w;
   cmd_begin(&w);
   if (to_device) {
      SVGA3dCmdUpdateGBSurface *c = cmd_add(&w, SVGA_3D_CMD_UPDATE_GB_SURFACE, sizeof(*c));
      c->sid = sid;
   } else {
      SVGA3dCmdReadbackGBSurface *c = cmd_add(&w, SVGA_3D_CMD_READBACK_GB_SURFACE, sizeof(*c));
      c->sid = sid;
   }
   const int r = cmd_run(&w, SVGA3D_INVALID_ID, to_device ? "import update" : "import readback");
   simple_mtx_unlock(&vmw_mtx);
   return r;
}

void *
sym_vmw_mmap(int fd, uint64_t offset, size_t size)
{
   if (fd != FD_VMW || !V.v)
      return (void *)-1;
   if (offset & IMPORT_MAP) {   /* an import's buffer: its own memory */
      const struct bo *b = bo_of((uint32_t)(offset & 0xffffffffu));
      return b && b->cpu ? b->cpu : (void *)-1;
   }
   if (offset + size > V.info.pool_bytes)
      return (void *)-1;
   return (uint8_t *)V.info.pool_base + offset;
}

void
sym_vmw_munmap(void *p, size_t size)
{
   (void)p; (void)size;   /* the pool stays mapped as long as the display lives */
}

/* the screen: the host GPU if this VM has one, else -1 */
int
sym_vmw_open(void)
{
   simple_mtx_lock(&vmw_mtx);
   if (V.v) {
      V.refs++;
      simple_mtx_unlock(&vmw_mtx);
      return FD_VMW;
   }
   symvmw_t *v = symvmw_open();
   if (!v) {
      simple_mtx_unlock(&vmw_mtx);
      return -1;
   }
   symvmw_info(v, &V.info);
   V.v = v;
   V.arena = V.info.arena_base;
   V.arena_bytes = V.info.arena_bytes;
   V.bos = calloc(V.info.mob_count, sizeof(struct bo));
   V.surfs = calloc(V.info.surf_count, sizeof(struct surf));
   V.ctxs = calloc(V.info.ctx_count, sizeof(struct ctx));
   V.nfree = 1;
   V.free_list[0].first = 0;
   V.free_list[0].pages = V.info.pool_bytes / PAGE;
   if (!V.bos || !V.surfs || !V.ctxs || symvmw_devcaps(v, V.devcaps, SYMVMW_DEVCAPS) != 0) {
      free(V.bos); free(V.surfs); free(V.ctxs);
      V.bos = NULL; V.surfs = NULL; V.ctxs = NULL;
      symvmw_close(v);
      V.v = NULL;
      simple_mtx_unlock(&vmw_mtx);
      return -1;
   }
   V.refs = 1;
   vlog("svga: SVGA3D device up: caps %08x/%08x, DX %u SM4.1 %u SM5 %u, MOB ids %u+%u, surface ids %u+%u, pool %u MB, arena %u KB",
        V.info.caps, V.info.cap2, V.info.devcap_dx, V.info.devcap_sm41, V.info.devcap_sm5, V.info.mob_base, V.info.mob_count,
        V.info.surf_base, V.info.surf_count, V.info.pool_bytes >> 20, V.info.arena_bytes >> 10);
   simple_mtx_unlock(&vmw_mtx);
   return FD_VMW;
}

void
sym_vmw_close(int fd)
{
   if (fd != FD_VMW)
      return;
   simple_mtx_lock(&vmw_mtx);
   if (V.v && --V.refs == 0) {
      vlog("svga: closing: %u command buffers, %u errors, %u buffers created", V.submits, V.errors, V.creates);
      symvmw_close(V.v);   /* (the kernel driver destroys what is still defined) */
      free(V.bos); free(V.surfs); free(V.ctxs);
      memset(&V, 0, sizeof(V));
   }
   simple_mtx_unlock(&vmw_mtx);
}

/* ---- scan-out takeover (S6) ---------------------------------------------------------------------------------------------------
 * A full-screen GL window need not go through the window server at all: its render target (created with the scan-out bind: a surface
 * the device can bind to a screen target) is defined as the VM's screen target, and each frame is one UPDATE_GB_SCREENTARGET -- no
 * read-back, no copy, no window server. Command buffers run in order, so the update follows the frame's rendering by itself (no fence
 * wait). The kernel's display driver takes the screen target down again the moment the window server reports a changed rectangle
 * (symvmw_scanout: 1 while it is still the display); the caller then presents through the window server as before. */

uint32_t
sym_vmw_surface_id(struct svga_winsys_surface *s)
{
   return s ? vmw_svga_winsys_surface(s)->sid : SVGA3D_INVALID_ID;
}

/* the display mode the kernel driver set through the SVGA registers, 0x0 if it does not drive the display */
void
sym_vmw_display_size(unsigned *w, unsigned *h)
{
   *w = V.v ? V.info.display_w : 0;
   *h = V.v ? V.info.display_h : 0;
}

static struct {
   uint32_t sid;
   unsigned w, h;
} scanout;

static void
scanout_destroy_locked(void)
{
   if (!scanout.sid)
      return;
   struct cmdw w;
   cmd_begin(&w);
   ((SVGA3dCmdDestroyGBScreenTarget *)cmd_add(&w, SVGA_3D_CMD_DESTROY_GB_SCREENTARGET, sizeof(SVGA3dCmdDestroyGBScreenTarget)))->stid = 0;
   cmd_run(&w, SVGA3D_INVALID_ID, "screen target destroy");
   scanout.sid = 0;
}

/* show surface sid (w x h) as the display; 0 = it is, <0 = it is not (not the display's size, the display driver took it down, a command failed) */
int
sym_vmw_scanout_present(uint32_t sid, unsigned w, unsigned h)
{
   if (!V.v || sid == SVGA3D_INVALID_ID)
      return -ENODEV;
   simple_mtx_lock(&vmw_mtx);
   int r = 0;
   if (w != V.info.display_w || h != V.info.display_h) {
      r = -EINVAL;
   } else if (scanout.sid && symvmw_scanout(V.v, 2) != 1) {
      scanout.sid = 0;   /* the display driver took it down: the window server drew something */
      r = -EBUSY;
   } else {
      struct cmdw c;
      cmd_begin(&c);
      if (scanout.sid != sid || scanout.w != w || scanout.h != h) {
         if (scanout.sid)
            scanout_destroy_locked();
         cmd_begin(&c);
         symvmw_scanout(V.v, 1);
         SVGA3dCmdDefineGBScreenTarget *d = cmd_add(&c, SVGA_3D_CMD_DEFINE_GB_SCREENTARGET, sizeof(*d));
         memset(d, 0, sizeof(*d));
         d->stid = 0;
         d->width = w;
         d->height = h;
         d->flags = SVGA_STFLAG_PRIMARY;
         SVGA3dCmdBindGBScreenTarget *b = cmd_add(&c, SVGA_3D_CMD_BIND_GB_SCREENTARGET, sizeof(*b));
         memset(b, 0, sizeof(*b));
         b->stid = 0;
         b->image.sid = sid;
         scanout.sid = sid;
         scanout.w = w;
         scanout.h = h;
         vlog("svga: scan-out takeover: surface %u as the %ux%u screen target", sid, w, h);
      }
      SVGA3dCmdUpdateGBScreenTarget *u = cmd_add(&c, SVGA_3D_CMD_UPDATE_GB_SCREENTARGET, sizeof(*u));
      memset(u, 0, sizeof(*u));
      u->stid = 0;
      u->rect.w = w;
      u->rect.h = h;
      r = cmd_run(&c, SVGA3D_INVALID_ID, "screen target update");
      if (r != 0) {
         symvmw_scanout(V.v, 0);
         scanout.sid = 0;
      }
   }
   simple_mtx_unlock(&vmw_mtx);
   return r;
}

/* a zero-copy import (the composer's target) as the display: the screen target over its GB surface. Not shown (another size, the display
   driver took the screen target back): the frame is read back into the import's memory at once, so the caller can copy it as before */
int
sym_vmw_present_import(int prime_fd, unsigned w, unsigned h)
{
   const uint32_t sid = (uint32_t)prime_fd - PRIME_BASE;
   if (!V.v || prime_fd < PRIME_BASE || sid < V.info.surf_base || sid >= V.info.surf_base + V.info.surf_count)
      return -EINVAL;
   struct surf *sf = &V.surfs[sid - V.info.surf_base];
   sf->shown = 1;
   const int r = sym_vmw_scanout_present(sid, w, h);
   if (r != 0) {
      static int n;
      if (n++ < 8)
         vlog("svga: import %u not shown (%d): read back", sid, r);
      sf->shown = 0;
      sym_vmw_surface_sync(prime_fd, 0);
   }
   return r;
}

/* give the display back (the window left full-screen, the surface is going away) */
void
sym_vmw_scanout_end(void)
{
   if (!V.v)
      return;
   simple_mtx_lock(&vmw_mtx);
   if (scanout.sid) {
      if (symvmw_scanout(V.v, 2) == 1)
         scanout_destroy_locked();
      scanout.sid = 0;
      symvmw_scanout(V.v, 0);
   }
   simple_mtx_unlock(&vmw_mtx);
}
