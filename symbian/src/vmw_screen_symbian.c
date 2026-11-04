/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* vmw_screen_symbian.c -- OURS (S3): replaces Mesa's vmw_screen.c (vmw_winsys_create / vmw_winsys_destroy). Mesa keys its winsys screens by the DRM
 * device number (fstat on the fd) so that two users of one device share a screen, and duplicates the fd; there is one emulated device here
 * (drm_symbian_vmw.c, a fake fd), so the screen is a singleton and the fd is kept as it is. The rest is Mesa's own sequence. */
#include "vmw_screen.h"
#include "vmw_fence.h"
#include "vmw_context.h"
#include "vmwgfx_drm.h"
#include "xf86drm.h"

#include "util/u_memory.h"
#include "util/u_debug.h"
#include "util/os_misc.h"
#include <string.h>

static struct vmw_winsys_screen *the_vws;

struct vmw_winsys_screen *
vmw_winsys_create(int fd)
{
   struct vmw_winsys_screen *vws;
   const char *getenv_val;

   if (the_vws) {
      the_vws->open_count++;
      return the_vws;
   }

   vws = CALLOC_STRUCT(vmw_winsys_screen);
   if (!vws)
      goto out_no_vws;

   vws->open_count = 1;
   vws->ioctl.drm_fd = fd;
   vws->force_coherent = false;
   if (!vmw_ioctl_init(vws))
      goto out_no_ioctl;

   vws->base.have_gb_dma = !vws->force_coherent;
   vws->base.max_mob_memory_mib = vws->ioctl.max_mob_memory >> 20;
   vws->base.need_to_rebind_resources = false;
   vws->base.have_transfer_from_buffer_cmd = vws->base.have_vgpu10;
   vws->base.have_constant_buffer_offset_cmd = vws->ioctl.have_drm_2_20 && vws->base.have_sm5;
   vws->base.have_index_vertex_buffer_offset_cmd = false;
   vws->base.have_rasterizer_state_v2_cmd = vws->ioctl.have_drm_2_20 && vws->base.have_sm5;

   getenv_val = os_get_option("SVGA_FORCE_KERNEL_UNMAPS");
   vws->cache_maps = !getenv_val || strcmp(getenv_val, "0") == 0;
   vws->fence_ops = vmw_fence_ops_create(vws);
   if (!vws->fence_ops)
      goto out_no_fence_ops;

   if (!vmw_pools_init(vws))
      goto out_no_pools;

   if (!vmw_winsys_screen_init_svga(vws))
      goto out_no_svga;

   cnd_init(&vws->cs_cond);
   mtx_init(&vws->cs_mutex, mtx_plain);

   the_vws = vws;
   return vws;
out_no_svga:
   vmw_pools_cleanup(vws);
out_no_pools:
   vws->fence_ops->destroy(vws->fence_ops);
out_no_fence_ops:
   vmw_ioctl_cleanup(vws);
out_no_ioctl:
   FREE(vws);
out_no_vws:
   return NULL;
}

void
vmw_winsys_destroy(struct vmw_winsys_screen *vws)
{
   if (--vws->open_count == 0) {
      if (vws->swc)
         vmw_swc_unref(vws->swc);
      the_vws = NULL;
      vmw_pools_cleanup(vws);
      vws->fence_ops->destroy(vws->fence_ops);
      vmw_ioctl_cleanup(vws);
      mtx_destroy(&vws->cs_mutex);
      cnd_destroy(&vws->cs_cond);
      FREE(vws);
   }
}
