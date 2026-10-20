/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* xf86drm.h -- OURS (GPU rung G4): the libdrm surface Mesa's lima driver (and gallium's renderonly) uses, declared
 * for Symbian. There is no DRM: every function is implemented by tools/mesa/symbian/src/drm_symbian.c over our Mali
 * kernel driver (RMaliChannel). Only what lima calls is here. Shadows the host's /usr/include/libdrm/xf86drm.h (that
 * include dir is dropped by build_mesa_symbian.py). */
#ifndef SYMBIAN_XF86DRM_H
#define SYMBIAN_XF86DRM_H

#include <stdint.h>
#include <stdlib.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_IOCTL_BASE 'd'
#ifndef DRM_IOCTL_NR
#define DRM_IOCTL_NR(n) ((n) & 0xff)
#endif

#define DRM_CLOEXEC 0        /* (fake fds: nothing to inherit or share) */
#define DRM_RDWR 0

#define DRM_BUS_PCI 0
#define DRM_BUS_USB 1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X 3

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2
#define DRM_NODE_MAX 3

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      void *pci;
      void *usb;
      drmPlatformDeviceInfoPtr platform;
      void *host1x;
   } businfo;
   union {
      void *pci;
      void *usb;
      drmPlatformDeviceInfoPtr platform;
      void *host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/* --- the Symbian side of the shim (drm_symbian.c) ----------------------------------------------------------- */

/* open the Mali: a fake DRM fd (>= 0) or -1 when there is no GPU */
int sym_drm_open(void);
void sym_drm_close(int fd);
/* a shared chunk of this process (handle, page-aligned range, its CPU address) mapped for the GPU: a fake "dma-buf
 * fd" for drmPrimeFDToHandle / resource_from_handle(WINSYS_HANDLE_TYPE_FD) */
int sym_drm_import(int fd, int chunk_handle, uint32_t offset, uint32_t size, void *cpu);
/* G6b: the chunk is found by a (page aligned) address in this process, e.g. the pixel data of an RSgImage */
int sym_drm_import_addr(int fd, const void *addr, uint32_t size);
void sym_drm_unimport(int fd, int prime_fd);
/* the GPU address of an imported range (cache maintenance needs it) */
uint32_t sym_drm_import_va(int fd, int prime_fd);
/* CPU cache maintenance on [offset, offset + size) of an import: op = TMaliCacheOp (0 before a GPU read, 1 before
 * a GPU write, 2 after a GPU write) */
int sym_drm_cache_sync(int fd, int prime_fd, uint32_t offset, uint32_t size, int op);
/* lima_bo.c's lseek(prime_fd, 0, SEEK_END) and mmap/munmap, lima_fence.c/lima_job.c's close() of sync files
 * (patched in build_mesa_symbian.py) */
long sym_drm_fd_size(int prime_fd);
void *sym_drm_mmap(int fd, uint64_t offset, size_t size);
void sym_drm_munmap(void *p, size_t size);
void sym_drm_close_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif
