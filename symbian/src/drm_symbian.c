/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* drm_symbian.c -- OURS (GPU rung G4): the lima DRM uAPI emulated
 * over our Mali kernel driver, so Mesa's lima driver (src/gallium/drivers/lima) runs unmodified (two one-line
 * patches aside: lseek/mmap of BOs, build_mesa_symbian.py).
 *
 * What Linux keeps in the kernel is kept here, in the process:
 *   GEM objects      -> sub-allocations of the channel's ARENA (a shared chunk the GPU sees at a fixed address),
 *                       committed from the kernel in 4 MB steps; an imported surface chunk is a BO too (sym_drm_import)
 *   job scheduling   -> one job per pipe in flight (the driver's rule); a submit first waits for the previous job of
 *                       its pipe, then for the jobs of the other pipe that still touch its BOs (the implicit sync
 *                       DRM derives from BO reservations: the PP reads what the GP wrote)
 *   syncobj / sync file / dma-fence -> a point on the two job timelines: {GP seqno, PP seqno} (0 = signalled)
 * fds are fake (>= 0x5A000000: a stray close() gets EBADF from P.I.P.S., nothing else). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "libsync.h"
#include "drm-uapi/lima_drm.h"
#include "symmali.h"
#include "util/os_time.h"

#define FD_BASE 0x5A000000
#define FD_SYNC 0x1000
#define FD_IMPORT 0x2000
#define MAX_BO 4096
#define MAX_SYNCOBJ 64
#define MAX_SYNCFD 256
#define MAX_IMPORT 16
#define MAX_FREE 1024
#define COMMIT_STEP (4u << 20)
#define PAGE 4096u

struct syncpt {
   uint32_t seq[2];           /* per pipe; 0 = nothing to wait for */
};

struct bo {
   int used;
   uint32_t size, va;
   void *cpu;
   uint32_t arena_off;        /* arena BOs */
   int import;                /* imported: the import index + 1, else 0 */
   uint32_t last_any[2], last_write[2];   /* the newest job of each pipe that reads / writes it */
};

struct import {
   int used;
   uint32_t va, size;
   void *cpu;
   int chunk_handle;
   uint32_t offset;
   int bo;                    /* BO handle lima holds for it, 0 = none */
};

struct range {
   uint32_t off, size;
};

static struct dev {
   symmali_t *m;
   symmali_info_t info;
   uint32_t committed;
   struct range free_list[MAX_FREE];
   int nfree;
   struct bo bos[MAX_BO + 1];             /* handle = index, 0 unused */
   struct syncpt syncobj[MAX_SYNCOBJ + 1];
   int syncobj_used[MAX_SYNCOBJ + 1];
   struct syncpt syncfd[MAX_SYNCFD];
   int syncfd_used[MAX_SYNCFD];
   struct import imports[MAX_IMPORT];
   uint32_t submitted[2], completed[2];
   int errors;
} the_dev;

static void
shimlog(const char *fmt, ...)
{
   char buf[176];
   va_list ap;
   va_start(ap, fmt);
   /* the phone log store keeps the "PH:" lines of user threads; the plain "[symdrm]" ones never showed up in recent boots */
   buf[0] = 'P'; buf[1] = 'H'; buf[2] = ':'; buf[3] = ' ';
   vsnprintf(buf + 4, sizeof(buf) - 4, fmt, ap);
   va_end(ap);
   symmali_log(buf);
}

static struct dev *
dev_of(int fd)
{
   if (fd != FD_BASE || !the_dev.m)
      return NULL;
   return &the_dev;
}

/* ---- the job timelines ------------------------------------------------------------------------------------- */

/* the pipe's job in flight finishes (or, polling, is still running: 1) */
static int
pipe_finish(struct dev *d, int pipe, int poll)
{
   if (d->submitted[pipe] == d->completed[pipe])
      return 0;
   int r = symmali_wait(d->m, pipe, poll);
   if (r == 1)
      return 1;
   d->completed[pipe] = d->submitted[pipe];
   if (r != 0 && d->errors++ < 8)
      shimlog("[symdrm] %s job #%u failed: %d", pipe ? "PP" : "GP", d->completed[pipe], r);
   return 0;
}

static int
seq_done(struct dev *d, int pipe, uint32_t seq)
{
   return seq == 0 || (int32_t)(d->completed[pipe] - seq) >= 0;
}

/* wait for a timeline point; timeout_ns < 0 = forever, 0 = poll; -ETIMEDOUT when it is not there yet */
static int
wait_point(struct dev *d, const struct syncpt *pt, int64_t timeout_ns)
{
   for (int p = 0; p < 2; p++) {
      if (seq_done(d, p, pt->seq[p]))
         continue;
      /* the only job of this pipe that can be unfinished is the one in flight */
      if (pipe_finish(d, p, timeout_ns == 0))
         return -ETIMEDOUT;
   }
   return 0;
}

static int64_t
rel_timeout(int64_t abs_ns)
{
   if (abs_ns <= 0)
      return 0;
   if (abs_ns == INT64_MAX)
      return -1;
   int64_t now = os_time_get_nano();
   return abs_ns > now ? abs_ns - now : 0;
}

/* ---- the arena ---------------------------------------------------------------------------------------------- */

static int
arena_alloc(struct dev *d, uint32_t size, uint32_t *off)
{
   size = (size + PAGE - 1) & ~(PAGE - 1);
   for (int i = 0; i < d->nfree; i++) {
      struct range *r = &d->free_list[i];
      if (r->size < size)
         continue;
      uint32_t o = r->off;
      uint32_t end = o + size;
      if (end > d->committed) {
         uint32_t want = (end + COMMIT_STEP - 1) & ~(COMMIT_STEP - 1);
         if (want > d->info.arena_max)
            want = d->info.arena_max;
         int rc = symmali_commit(d->m, d->committed, want - d->committed);
         if (rc != 0) {
            shimlog("[symdrm] arena commit %u+%u failed: %d", d->committed, want - d->committed, rc);
            return -ENOMEM;
         }
         d->committed = want;
      }
      r->off += size;
      r->size -= size;
      if (r->size == 0) {
         memmove(r, r + 1, (d->nfree - i - 1) * sizeof(*r));
         d->nfree--;
      }
      *off = o;
      return 0;
   }
   return -ENOMEM;
}

static void
arena_free(struct dev *d, uint32_t off, uint32_t size)
{
   size = (size + PAGE - 1) & ~(PAGE - 1);
   int i = 0;
   while (i < d->nfree && d->free_list[i].off < off)
      i++;
   /* merge with the previous / next free range */
   if (i > 0 && d->free_list[i - 1].off + d->free_list[i - 1].size == off) {
      d->free_list[i - 1].size += size;
      if (i < d->nfree && d->free_list[i].off == off + size) {
         d->free_list[i - 1].size += d->free_list[i].size;
         memmove(&d->free_list[i], &d->free_list[i + 1], (d->nfree - i - 1) * sizeof(struct range));
         d->nfree--;
      }
      return;
   }
   if (i < d->nfree && d->free_list[i].off == off + size) {
      d->free_list[i].off = off;
      d->free_list[i].size += size;
      return;
   }
   if (d->nfree == MAX_FREE) {
      shimlog("[symdrm] free list full: %u bytes at %u leaked", size, off);
      return;
   }
   memmove(&d->free_list[i + 1], &d->free_list[i], (d->nfree - i) * sizeof(struct range));
   d->free_list[i].off = off;
   d->free_list[i].size = size;
   d->nfree++;
}

/* ---- BOs ---------------------------------------------------------------------------------------------------- */

static int
bo_alloc_handle(struct dev *d)
{
   for (int h = 1; h <= MAX_BO; h++)
      if (!d->bos[h].used)
         return h;
   return 0;
}

static struct bo *
bo_of(struct dev *d, uint32_t handle)
{
   if (handle == 0 || handle > MAX_BO || !d->bos[handle].used)
      return NULL;
   return &d->bos[handle];
}

/* every job that touches the BO is finished (before it is freed or handed back) */
static void
bo_idle(struct dev *d, struct bo *b, int write)
{
   struct syncpt pt;
   for (int p = 0; p < 2; p++)
      pt.seq[p] = write ? b->last_any[p] : b->last_write[p];
   wait_point(d, &pt, -1);
}

/* One-off CPU read-speed probe of the (uncached, write-combining) arena memory, on the first big BO. Timed with User::FastCounter (24 MHz CNTPCT on this board -- HAL::EFastCounterFrequency wrongly said 1 MHz; os_time_get_nano is a
 * 1 ms tick and printf's %lld came out as garbage), 8 passes over 16 KB so that a fast load is still measurable; 32-bit values only. "PH: [symdrm] arena read probe" lines. */
extern unsigned symmali_fast_counter(void);

static void
arena_read_probe(char *p)
{
   enum { N = 4096, PASSES = 8 };
   volatile uint32_t *w = (volatile uint32_t *)p;
   static uint32_t cachedbuf[N];
   static volatile uint32_t gsink;
   uint32_t sum = 0;
   for (int round = 0; round < 2; round++) {
      unsigned t0 = symmali_fast_counter();
      for (int pass = 0; pass < PASSES; pass++)
         for (int i = 0; i < N; i++)
            sum += w[i];
      unsigned t1 = symmali_fast_counter();
      shimlog("[symdrm] arena read probe: %d x 32-bit volatile loads from the arena (round %d): %u ticks total = %u ns each", N * PASSES, round, t1 - t0, (t1 - t0) * 125u / 3u / (N * PASSES));
   }
   unsigned t0 = symmali_fast_counter();
   for (int pass = 0; pass < PASSES; pass++) {
      memcpy(cachedbuf, p, sizeof(cachedbuf));
      sum += cachedbuf[pass];
   }
   unsigned t1 = symmali_fast_counter();
   shimlog("[symdrm] arena read probe: memcpy of %d x %d bytes from the arena: %u ticks total = %u ns per 32-bit word", PASSES, (int)sizeof(cachedbuf), t1 - t0, (t1 - t0) * 125u / 3u / (N * PASSES));
   volatile uint8_t *b = (volatile uint8_t *)p;
   t0 = symmali_fast_counter();
   for (int pass = 0; pass < PASSES; pass++)
      for (int i = 0; i < 1024; i++)
         sum += b[i];
   t1 = symmali_fast_counter();
   shimlog("[symdrm] arena read probe: %d byte loads: %u ticks total = %u ns each", 1024 * PASSES, t1 - t0, (t1 - t0) * 125u / 3u / (1024 * PASSES));
   volatile uint32_t *c = (volatile uint32_t *)cachedbuf;
   t0 = symmali_fast_counter();
   for (int pass = 0; pass < PASSES; pass++)
      for (int i = 0; i < N; i++)
         sum += c[i];
   t1 = symmali_fast_counter();
   gsink = sum;
   shimlog("[symdrm] arena read probe: control, %d x 32-bit loads from the DLL's .bss buffer: %u ticks total = %u ns each (sum %u, first arena words %08x %08x)", N * PASSES, t1 - t0, (t1 - t0) * 125u / 3u / (N * PASSES), sum, (unsigned)w[0], (unsigned)w[1]);
   {	/* the same loop over a 16 KB buffer on this thread's STACK and over a malloc'd (process heap) buffer: is ANY user memory cached? */
      volatile uint32_t sbuf[N];
      for (int i = 0; i < N; i++)
         sbuf[i] = i;
      t0 = symmali_fast_counter();
      for (int pass = 0; pass < PASSES; pass++)
         for (int i = 0; i < N; i++)
            sum += sbuf[i];
      t1 = symmali_fast_counter();
      shimlog("[symdrm] arena read probe: %d x 32-bit loads from a 16 KB STACK buffer: %u ticks total = %u ns each", N * PASSES, t1 - t0, (t1 - t0) * 125u / 3u / (N * PASSES));
      volatile uint32_t *hb = (volatile uint32_t *)malloc(N * 4);
      if (hb) {
         for (int i = 0; i < N; i++)
            hb[i] = i;
         t0 = symmali_fast_counter();
         for (int pass = 0; pass < PASSES; pass++)
            for (int i = 0; i < N; i++)
               sum += hb[i];
         t1 = symmali_fast_counter();
         shimlog("[symdrm] arena read probe: %d x 32-bit loads from a malloc'd 16 KB buffer: %u ticks total = %u ns each", N * PASSES, t1 - t0, (t1 - t0) * 125u / 3u / (N * PASSES));
         free((void *)hb);
      }
   }
   {	/* pure ALU reference (no memory): 1,048,576 dependent adds + xors = about 2 cycles each on an in-order A53 */
      uint32_t x = sum | 1;
      t0 = symmali_fast_counter();
      for (int i = 0; i < (1 << 20); i++) {
         x += (x << 1) ^ (uint32_t)i;
         __asm__ volatile("" : "+r"(x));
      }
      t1 = symmali_fast_counter();
      gsink = x;
      shimlog("[symdrm] arena read probe: ALU reference, 1048576 iterations (add/shift/xor, no loads): %u ticks total @24 MHz (= %u ps per iteration; x %u)", t1 - t0, (t1 - t0) * 40u, x);
   }
}

static int
gem_create(struct dev *d, struct drm_lima_gem_create *req)
{
   int h = bo_alloc_handle(d);
   if (!h)
      return -ENOMEM;
   uint32_t off;
   int r = arena_alloc(d, req->size, &off);
   if (r)
      return r;
   struct bo *b = &d->bos[h];
   memset(b, 0, sizeof(*b));
   b->used = 1;
   b->size = (req->size + PAGE - 1) & ~(PAGE - 1);
   b->arena_off = off;
   b->va = d->info.arena_va + off;
   b->cpu = (char *)d->info.arena_base + off;
   {
      static int probed;
      if (!probed && b->size >= 262144) {
         probed = 1;
         arena_read_probe(b->cpu);
      }
   }
   req->handle = h;
   req->size = b->size;
   return 0;
}

static int
gem_close(struct dev *d, uint32_t handle)
{
   struct bo *b = bo_of(d, handle);
   if (!b)
      return -ENOENT;
   bo_idle(d, b, 1);
   if (b->import) {
      struct import *im = &d->imports[b->import - 1];
      if (im->used && im->bo == (int)handle)
         im->bo = 0;
   } else {
      arena_free(d, b->arena_off, b->size);
   }
   b->used = 0;
   return 0;
}

/* ---- submit ------------------------------------------------------------------------------------------------- */

static int
gem_submit(struct dev *d, struct drm_lima_gem_submit *req)
{
   const int pipe = req->pipe;
   if (pipe != LIMA_PIPE_GP && pipe != LIMA_PIPE_PP)
      return -EINVAL;
   const unsigned words = req->frame_size / 4;
   const unsigned want = pipe == LIMA_PIPE_GP ? sizeof(struct drm_lima_gp_frame) / 4
                                              : sizeof(struct drm_lima_m400_pp_frame) / 4;
   if (words != want)
      return -EINVAL;
   const struct drm_lima_gem_submit_bo *bos = (const struct drm_lima_gem_submit_bo *)(uintptr_t)req->bos;
   const uint32_t *frame = (const uint32_t *)(uintptr_t)req->frame;

   /* the pipe is free */
   pipe_finish(d, pipe, 0);
   /* explicit dependency */
   if (req->in_sync[0]) {
      if (req->in_sync[0] > MAX_SYNCOBJ || !d->syncobj_used[req->in_sync[0]])
         return -ENOENT;
      wait_point(d, &d->syncobj[req->in_sync[0]], -1);
   }
   /* implicit dependencies: the other pipe's unfinished job writing a BO we use, or using a BO we write */
   const int other = 1 - pipe;
   for (unsigned i = 0; i < req->nr_bos; i++) {
      struct bo *b = bo_of(d, bos[i].handle);
      if (!b)
         return -ENOENT;
      if (!seq_done(d, other, b->last_write[other]) ||
          ((bos[i].flags & LIMA_SUBMIT_BO_WRITE) && !seq_done(d, other, b->last_any[other])))
         pipe_finish(d, other, 0);
   }

   int r = symmali_submit(d->m, pipe, frame, words);
   if (r != 0) {
      shimlog("[symdrm] submit %s failed: %d", pipe ? "PP" : "GP", r);
      return -EINVAL;
   }
   const uint32_t seq = ++d->submitted[pipe];
   for (unsigned i = 0; i < req->nr_bos; i++) {
      struct bo *b = &d->bos[bos[i].handle];
      b->last_any[pipe] = seq;
      if (bos[i].flags & LIMA_SUBMIT_BO_WRITE)
         b->last_write[pipe] = seq;
   }
   if (req->out_sync && req->out_sync <= MAX_SYNCOBJ && d->syncobj_used[req->out_sync]) {
      d->syncobj[req->out_sync].seq[0] = 0;
      d->syncobj[req->out_sync].seq[1] = 0;
      d->syncobj[req->out_sync].seq[pipe] = seq;
   }
   return 0;
}

static int
gem_wait(struct dev *d, struct drm_lima_gem_wait *req)
{
   struct bo *b = bo_of(d, req->handle);
   if (!b)
      return -ENOENT;
   struct syncpt pt;
   for (int p = 0; p < 2; p++)
      pt.seq[p] = (req->op & LIMA_GEM_WAIT_WRITE) ? b->last_any[p] : b->last_write[p];
   return wait_point(d, &pt, rel_timeout(req->timeout_ns));
}

/* ---- the DRM entry points ----------------------------------------------------------------------------------- */

static int
fail(int err)
{
   errno = err < 0 ? -err : err;
   return -1;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
   struct dev *d = dev_of(fd);
   if (!d)
      return fail(EBADF);
   int r;
   switch (request) {
   case DRM_IOCTL_LIMA_GET_PARAM: {
      struct drm_lima_get_param *p = arg;
      switch (p->param) {
      case DRM_LIMA_PARAM_GPU_ID: p->value = DRM_LIMA_PARAM_GPU_ID_MALI400; break;
      case DRM_LIMA_PARAM_NUM_PP: p->value = d->info.num_pp; break;
      case DRM_LIMA_PARAM_GP_VERSION: p->value = d->info.gp_version; break;
      case DRM_LIMA_PARAM_PP_VERSION: p->value = d->info.pp_version; break;
      default: return fail(EINVAL);
      }
      return 0;
   }
   case DRM_IOCTL_LIMA_GEM_CREATE:
      r = gem_create(d, arg);
      break;
   case DRM_IOCTL_LIMA_GEM_INFO: {
      struct drm_lima_gem_info *p = arg;
      struct bo *b = bo_of(d, p->handle);
      if (!b)
         return fail(ENOENT);
      p->va = b->va;
      p->offset = (uint64_t)p->handle << 12;
      return 0;
   }
   case DRM_IOCTL_LIMA_GEM_SUBMIT:
      r = gem_submit(d, arg);
      break;
   case DRM_IOCTL_LIMA_GEM_WAIT:
      r = gem_wait(d, arg);
      break;
   case DRM_IOCTL_LIMA_CTX_CREATE: {
      struct drm_lima_ctx_create *p = arg;
      p->id = 1;
      return 0;
   }
   case DRM_IOCTL_LIMA_CTX_FREE:
      return 0;
   case DRM_IOCTL_GEM_CLOSE: {
      struct drm_gem_close *p = arg;
      r = gem_close(d, p->handle);
      break;
   }
   default:
      return fail(ENOSYS);
   }
   return r ? fail(r) : 0;
}

static drmVersion the_version = { 1, 0, 0, 4, (char *)"lima", 0, (char *)"", 0, (char *)"" };

drmVersionPtr
drmGetVersion(int fd)
{
   return dev_of(fd) ? &the_version : NULL;
}

void
drmFreeVersion(drmVersionPtr v)
{
   (void)v;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd; (void)flags;
   *device = NULL;
   return -ENODEV;
}

void
drmFreeDevice(drmDevicePtr *device)
{
   (void)device;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   return -ENOSYS;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   struct dev *d = dev_of(fd);
   int idx = prime_fd - (FD_BASE + FD_IMPORT);
   if (!d || idx < 0 || idx >= MAX_IMPORT || !d->imports[idx].used)
      return -EBADF;
   struct import *im = &d->imports[idx];
   if (!im->bo) {
      int h = bo_alloc_handle(d);
      if (!h)
         return -ENOMEM;
      struct bo *b = &d->bos[h];
      memset(b, 0, sizeof(*b));
      b->used = 1;
      b->size = im->size;
      b->va = im->va;
      b->cpu = im->cpu;
      b->import = idx + 1;
      im->bo = h;
   }
   *handle = im->bo;
   return 0;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct dev *d = dev_of(fd);
   if (!d)
      return -EBADF;
   for (int h = 1; h <= MAX_SYNCOBJ; h++) {
      if (d->syncobj_used[h])
         continue;
      d->syncobj_used[h] = 1;
      d->syncobj[h].seq[0] = d->syncobj[h].seq[1] = 0;
      (void)flags;   /* unsignalled and signalled are the same thing for a point nobody has set */
      *handle = h;
      return 0;
   }
   return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct dev *d = dev_of(fd);
   if (!d || handle == 0 || handle > MAX_SYNCOBJ)
      return -EINVAL;
   d->syncobj_used[handle] = 0;
   return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
               uint32_t *first_signaled)
{
   struct dev *d = dev_of(fd);
   (void)flags;
   if (!d)
      return -EBADF;
   for (unsigned i = 0; i < num_handles; i++) {
      if (handles[i] == 0 || handles[i] > MAX_SYNCOBJ || !d->syncobj_used[handles[i]])
         return -ENOENT;
      int r = wait_point(d, &d->syncobj[handles[i]], rel_timeout(timeout_nsec));
      if (r)
         return r;
      if (first_signaled)
         *first_signaled = i;
   }
   return 0;
}

static int
syncfd_new(struct dev *d, const struct syncpt *pt)
{
   for (int i = 0; i < MAX_SYNCFD; i++) {
      if (d->syncfd_used[i])
         continue;
      d->syncfd_used[i] = 1;
      d->syncfd[i] = *pt;
      return FD_BASE + FD_SYNC + i;
   }
   shimlog("[symdrm] out of sync files");
   return -1;
}

static struct syncpt *
syncfd_of(struct dev *d, int fd)
{
   int i = fd - (FD_BASE + FD_SYNC);
   if (i < 0 || i >= MAX_SYNCFD || !d->syncfd_used[i])
      return NULL;
   return &d->syncfd[i];
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   struct dev *d = dev_of(fd);
   if (!d || handle == 0 || handle > MAX_SYNCOBJ || !d->syncobj_used[handle])
      return -EINVAL;
   struct syncpt *pt = syncfd_of(d, sync_file_fd);
   if (!pt)
      return -EBADF;
   d->syncobj[handle] = *pt;
   return 0;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   struct dev *d = dev_of(fd);
   if (!d || handle == 0 || handle > MAX_SYNCOBJ || !d->syncobj_used[handle])
      return -EINVAL;
   *sync_file_fd = syncfd_new(d, &d->syncobj[handle]);
   return *sync_file_fd < 0 ? -ENOMEM : 0;
}

/* ---- libsync ------------------------------------------------------------------------------------------------ */

int
sync_wait(int fd, int timeout_ms)
{
   struct dev *d = &the_dev;
   struct syncpt *pt = syncfd_of(d, fd);
   if (!pt)
      return fail(EBADF);
   int r = wait_point(d, pt, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000000);
   return r ? fail(r) : 0;
}

int
sync_merge(const char *name, int fd1, int fd2)
{
   (void)name;
   struct dev *d = &the_dev;
   struct syncpt *a = syncfd_of(d, fd1), *b = syncfd_of(d, fd2);
   if (!a || !b)
      return fail(EBADF);
   struct syncpt m;
   for (int p = 0; p < 2; p++)
      m.seq[p] = (int32_t)(a->seq[p] - b->seq[p]) >= 0 ? a->seq[p] : b->seq[p];
   return syncfd_new(d, &m);
}

int
sync_accumulate(const char *name, int *fd, int fd2)
{
   struct dev *d = &the_dev;
   if (*fd < 0) {
      struct syncpt *b = syncfd_of(d, fd2);
      if (!b)
         return fail(EBADF);
      *fd = syncfd_new(d, b);
      return *fd < 0 ? -1 : 0;
   }
   int m = sync_merge(name, *fd, fd2);
   if (m < 0)
      return -1;
   sym_drm_close_fd(*fd);
   *fd = m;
   return 0;
}

/* ---- the Symbian side --------------------------------------------------------------------------------------- */

int
sym_drm_open(void)
{
   struct dev *d = &the_dev;
   if (d->m)
      return FD_BASE;
   symmali_t *m = symmali_open();
   if (!m)
      return -1;
   memset(d, 0, sizeof(*d));
   d->m = m;
   symmali_info(m, &d->info);
   d->free_list[0].off = 0;
   d->free_list[0].size = d->info.arena_max;
   d->nfree = 1;
   shimlog("[symdrm] Mali GP %08x PP %08x x%d, arena %u MB at %08x", d->info.gp_version, d->info.pp_version,
        d->info.num_pp, d->info.arena_max >> 20, d->info.arena_va);
   return FD_BASE;
}

void
sym_drm_close(int fd)
{
   struct dev *d = dev_of(fd);
   if (!d)
      return;
   for (int p = 0; p < 2; p++)
      pipe_finish(d, p, 0);
   for (int i = 0; i < MAX_IMPORT; i++)
      if (d->imports[i].used)
         symmali_unimport(d->m, d->imports[i].va);
   symmali_close(d->m);
   memset(d, 0, sizeof(*d));
}

int
sym_drm_import(int fd, int chunk_handle, uint32_t offset, uint32_t size, void *cpu)
{
   struct dev *d = dev_of(fd);
   if (!d)
      return -1;
   for (int i = 0; i < MAX_IMPORT; i++) {
      struct import *im = &d->imports[i];
      if (im->used)
         continue;
      uint32_t va = 0;
      int r = symmali_import(d->m, chunk_handle, offset, size, &va);
      if (r != 0) {
         shimlog("[symdrm] import of chunk %d +%u (%u bytes) failed: %d", chunk_handle, offset, size, r);
         return -1;
      }
      memset(im, 0, sizeof(*im));
      im->used = 1;
      im->va = va;
      im->size = size;
      im->cpu = cpu;
      im->chunk_handle = chunk_handle;
      im->offset = offset;
      return FD_BASE + FD_IMPORT + i;
   }
   return -1;
}

int
sym_drm_import_addr(int fd, const void *addr, uint32_t size)
{
   struct dev *d = dev_of(fd);
   if (!d)
      return -1;
   for (int i = 0; i < MAX_IMPORT; i++) {
      struct import *im = &d->imports[i];
      if (im->used)
         continue;
      uint32_t va = 0;
      int r = symmali_import_addr(d->m, addr, size, &va);
      if (r != 0) {
         shimlog("[symdrm] import of address %p (%u bytes) failed: %d", addr, size, r);
         return -1;
      }
      memset(im, 0, sizeof(*im));
      im->used = 1;
      im->va = va;
      im->size = size;
      im->cpu = (void *)addr;
      im->chunk_handle = -1;
      return FD_BASE + FD_IMPORT + i;
   }
   return -1;
}

static struct import *
import_of(struct dev *d, int prime_fd)
{
   int i = prime_fd - (FD_BASE + FD_IMPORT);
   if (!d || i < 0 || i >= MAX_IMPORT || !d->imports[i].used)
      return NULL;
   return &d->imports[i];
}

void
sym_drm_unimport(int fd, int prime_fd)
{
   struct dev *d = dev_of(fd);
   struct import *im = import_of(d, prime_fd);
   if (!im)
      return;
   if (im->bo) {
      bo_idle(d, &d->bos[im->bo], 1);
      d->bos[im->bo].import = 0;      /* lima still holds the handle: it closes it as a plain BO */
      d->bos[im->bo].used = 0;
   }
   symmali_unimport(d->m, im->va);
   im->used = 0;
}

uint32_t
sym_drm_import_va(int fd, int prime_fd)
{
   struct import *im = import_of(dev_of(fd), prime_fd);
   return im ? im->va : 0;
}

int
sym_drm_cache_sync(int fd, int prime_fd, uint32_t offset, uint32_t size, int op)
{
   struct dev *d = dev_of(fd);
   struct import *im = import_of(d, prime_fd);
   static int said;
   if (!im || offset + size > im->size) {
      if (said++ < 8)
         shimlog("[symdrm] cache sync %d: +%u (%u bytes) outside import %d (%u bytes)", op, offset, size, prime_fd,
                 im ? im->size : 0);
      return -1;
   }
   int r = symmali_cache_sync(d->m, im->va + offset, size, op);
   if (r != 0 && said++ < 8)
      shimlog("[symdrm] cache sync %d of va %08x (%u bytes) failed: %d", op, im->va + offset, size, r);
   return r;
}

long
sym_drm_fd_size(int prime_fd)
{
   struct import *im = import_of(&the_dev, prime_fd);
   return im ? (long)im->size : -1;
}

void *
sym_drm_mmap(int fd, uint64_t offset, size_t size)
{
   struct dev *d = dev_of(fd);
   struct bo *b = d ? bo_of(d, (uint32_t)(offset >> 12)) : NULL;
   if (!b || size > b->size)
      return (void *)-1;
   return b->cpu;
}

void
sym_drm_munmap(void *p, size_t size)
{
   (void)p; (void)size;
}

void
sym_drm_close_fd(int fd)
{
   int i = fd - (FD_BASE + FD_SYNC);
   if (i >= 0 && i < MAX_SYNCFD)
      the_dev.syncfd_used[i] = 0;
}
