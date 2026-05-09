/*
 * i915_drm.h — Haiku shim for Mesa's drm-uapi/i915_drm.h.
 *
 * Re-exports all struct definitions and ioctl numbers from our
 * DRM shim header. Mesa crocus includes this as "drm-uapi/i915_drm.h".
 */
#ifndef I915_DRM_H
#define I915_DRM_H

#include "haiku_drm_intel.h"

/* Additional ioctl numbers crocus may use */
#ifndef DRM_IOCTL_I915_GEM_SET_CACHING
#define DRM_IOCTL_I915_GEM_SET_CACHING  0x5f  /* reuse SET_DOMAIN slot */
#endif

#ifndef DRM_IOCTL_I915_GEM_MADVISE
#define DRM_IOCTL_I915_GEM_MADVISE      0x66
#endif

#ifndef DRM_IOCTL_I915_GEM_USERPTR
#define DRM_IOCTL_I915_GEM_USERPTR      0x75
#endif

#ifndef DRM_IOCTL_GEM_OPEN
#define DRM_IOCTL_GEM_OPEN              0x0b
#endif

/* Additional param IDs used by crocus */
#ifndef I915_PARAM_HAS_LLC
#define I915_PARAM_HAS_LLC              17
#endif
#ifndef I915_PARAM_HAS_WAIT_TIMEOUT
#define I915_PARAM_HAS_WAIT_TIMEOUT     19
#endif
#ifndef I915_PARAM_HAS_EXEC_NO_RELOC
#define I915_PARAM_HAS_EXEC_NO_RELOC    25
#endif
#ifndef I915_PARAM_HAS_EXEC_HANDLE_LUT
#define I915_PARAM_HAS_EXEC_HANDLE_LUT  26
#endif
#ifndef I915_PARAM_HAS_EXEC_BATCH_FIRST
#define I915_PARAM_HAS_EXEC_BATCH_FIRST 48
#endif
#ifndef I915_PARAM_HAS_EXEC_FENCE
#define I915_PARAM_HAS_EXEC_FENCE       44
#endif

/* Exec ring selector */
#ifndef I915_EXEC_RING_MASK
#define I915_EXEC_RING_MASK     0x3f
#define I915_EXEC_DEFAULT       0
#define I915_EXEC_BLT           (2 << 0)
#define I915_EXEC_BSD           (1 << 0)
#endif

/* GEM caching modes */
#define I915_CACHING_NONE       0
#define I915_CACHING_CACHED     1
#define I915_CACHING_DISPLAY    2

/* MADVISE */
#define I915_MADV_WILLNEED      0
#define I915_MADV_DONTNEED      1

struct drm_i915_gem_caching {
	uint32_t handle;
	uint32_t caching;
};

struct drm_i915_gem_madvise {
	uint32_t handle;
	uint32_t madv;
	uint32_t retained;  /* out */
	uint32_t pad;
};

struct drm_i915_gem_userptr {
	uint64_t user_ptr;
	uint64_t user_size;
	uint32_t flags;
	uint32_t handle;    /* out */
};

struct drm_gem_open {
	char name[64];
	uint32_t handle;    /* out */
	uint64_t size;      /* out */
};

#endif /* I915_DRM_H */
