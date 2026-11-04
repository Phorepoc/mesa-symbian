/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* vmw_msg_symbian.c -- OURS (S3): replaces Mesa's vmw_msg.c, which talks to the hypervisor through the VMware backdoor (an `in` / `out` pair on I/O
 * port 0x5658) or, from DRM 2.17, the DRM_VMW_MSG ioctl. A Symbian process may not execute port I/O, and the vmwgfx emulation (drm_symbian_vmw.c)
 * has no message channel to the host; the winsys' "host log" lines go to the debug log instead. */
#include <stdio.h>
#include <string.h>

#include "svga_winsys.h"
#include "vmw_msg.h"
#include "symgl_native.h"

void
vmw_svga_winsys_host_log(struct svga_winsys_screen *sws, const char *log)
{
   (void)sws;
   char buf[160];
   snprintf(buf, sizeof(buf), "PH: svga host log: %s", log);
   symgl_log(buf);
}
