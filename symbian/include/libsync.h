/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */

/* libsync.h -- OURS (G4): Android/Linux sync-file helpers as lima_fence.c uses them. A "sync file" is a fake fd of
 * drm_symbian.c holding a {GP seqno, PP seqno} pair of our Mali driver's job timelines. */
#ifndef SYMBIAN_LIBSYNC_H
#define SYMBIAN_LIBSYNC_H

#ifdef __cplusplus
extern "C" {
#endif

void sym_drm_close_fd(int fd);   /* (lima_fence.c's close() of a fence fd is patched to this) */
int sync_wait(int fd, int timeout_ms);
int sync_merge(const char *name, int fd1, int fd2);
/* *fd = the merge of *fd and fd2 (fd2 is left to the caller); *fd < 0 = take a copy of fd2 */
int sync_accumulate(const char *name, int *fd, int fd2);

#ifdef __cplusplus
}
#endif

#endif
