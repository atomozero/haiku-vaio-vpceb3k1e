/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * DRM compatibility library for Haiku OS.
 *
 * Translates Linux DRM/i915 semantics to Haiku kernel ioctls.
 * Clean C API for Mesa crocus and other consumers.
 */

#ifndef LIBDRM_HAIKU_H
#define LIBDRM_HAIKU_H

#include <SupportDefs.h>

#ifdef __cplusplus
extern "C" {
#endif


/* -------------------------------------------------------------------------
 * Device management
 * ---------------------------------------------------------------------- */

int		drm_haiku_open(const char* device_path);
void	drm_haiku_close(int fd);
int		drm_haiku_get_version(int fd, uint32* version, uint32* generation,
			uint16* device_id);


/* -------------------------------------------------------------------------
 * GEM buffer management
 * ---------------------------------------------------------------------- */

int		drm_haiku_gem_create(int fd, uint64 size, uint32* handle);
int		drm_haiku_gem_close(int fd, uint32 handle);
int		drm_haiku_gem_mmap(int fd, uint32 handle, uint64 offset,
			uint64 size, void** addr);
int		drm_haiku_gem_set_tiling(int fd, uint32 handle,
			uint32 tiling_mode, uint32 stride);
int		drm_haiku_gem_get_tiling(int fd, uint32 handle,
			uint32* tiling_mode, uint32* swizzle_mode);
int		drm_haiku_gem_wait(int fd, uint32 handle, int64 timeout_ns);
int		drm_haiku_gem_busy(int fd, uint32 handle, uint32* busy);
int		drm_haiku_gem_set_domain(int fd, uint32 handle,
			uint32 read_domains, uint32 write_domain);


/* -------------------------------------------------------------------------
 * Batch execution
 * ---------------------------------------------------------------------- */

/* Opaque exec structures — use the igen_* types from ioctl_interface.h */
int		drm_haiku_execbuffer2(int fd, void* execbuf_args);


/* -------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------- */

int		drm_haiku_getparam(int fd, int32 param, int32* value);
int		drm_haiku_get_aperture(int fd, uint64* total, uint64* available);


/* -------------------------------------------------------------------------
 * Context management
 * ---------------------------------------------------------------------- */

int		drm_haiku_context_create(int fd, uint32* ctx_id);
int		drm_haiku_context_destroy(int fd, uint32 ctx_id);


/* -------------------------------------------------------------------------
 * Linux DRM compatibility layer (for Mesa)
 * ---------------------------------------------------------------------- */

/* Mesa expects standard drmIoctl(). This wraps our API. */
int		drmIoctl(int fd, unsigned long request, void* arg);
int		drmGetCap(int fd, uint64 capability, uint64* value);
int		drmSetClientCap(int fd, uint64 capability, uint64 value);


#ifdef __cplusplus
}
#endif

#endif /* LIBDRM_HAIKU_H */
