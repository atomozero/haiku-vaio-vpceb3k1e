/*
 * haiku_drm_intel.h — DRM shim for Mesa crocus on Haiku OS.
 *
 * Translates i915 DRM ioctls to Haiku intel_extreme driver calls.
 * Minimum viable set for crocus Gen5 (Ironlake).
 *
 * Usage: Mesa crocus calls intel_ioctl(fd, DRM_IOCTL_..., &args).
 * We intercept these via haiku_drm_ioctl() which does the translation.
 */

#ifndef HAIKU_DRM_INTEL_H
#define HAIKU_DRM_INTEL_H

#include <stdint.h>

/* ---- DRM ioctl numbers (matching Linux uapi) ---- */

#define DRM_IOCTL_BASE              'd'
#define DRM_IOCTL_I915_GETPARAM     0x46
#define DRM_IOCTL_I915_GEM_CREATE   0x5b
#define DRM_IOCTL_GEM_CLOSE         0x09
#define DRM_IOCTL_I915_GEM_MMAP     0x5e
#define DRM_IOCTL_I915_GEM_MMAP_GTT 0x64
#define DRM_IOCTL_I915_GEM_SET_DOMAIN 0x5f
#define DRM_IOCTL_I915_GEM_EXECBUFFER2 0x69
#define DRM_IOCTL_I915_GEM_BUSY     0x60
#define DRM_IOCTL_I915_GEM_WAIT     0x6c
#define DRM_IOCTL_I915_GEM_GET_APERTURE 0x63
#define DRM_IOCTL_I915_GEM_SET_TILING   0x61
#define DRM_IOCTL_I915_GEM_GET_TILING   0x62
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE 0x6d
#define DRM_IOCTL_I915_GEM_CONTEXT_DESTROY 0x6e
#define DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM 0x73
#define DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM 0x74
#define DRM_IOCTL_I915_GET_RESET_STATS 0x72

/* ---- DRM structs (matching Linux drm_i915_drm.h) ---- */

struct drm_i915_gem_create {
	uint64_t size;      /* in: allocation size */
	uint32_t handle;    /* out: GEM handle */
	uint32_t pad;
};

struct drm_gem_close {
	uint32_t handle;
	uint32_t pad;
};

struct drm_i915_gem_mmap {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;    /* in: offset within BO */
	uint64_t size;      /* in: bytes to map */
	uint64_t addr_ptr;  /* out: mapped address */
	uint64_t flags;
};

struct drm_i915_gem_busy {
	uint32_t handle;
	uint32_t busy;      /* out: 0=idle, nonzero=busy */
};

struct drm_i915_gem_set_domain {
	uint32_t handle;
	uint32_t read_domains;
	uint32_t write_domain;
};

struct drm_i915_getparam {
	int32_t param;
	int32_t *value;
};

struct drm_i915_gem_get_aperture {
	uint64_t aper_size;
	uint64_t aper_available_size;
};

struct drm_i915_gem_exec_object2 {
	uint32_t handle;
	uint32_t relocation_count;
	uint64_t relocs_ptr;
	uint64_t alignment;
	uint64_t offset;        /* GTT offset (in/out) */
	uint64_t flags;
	uint64_t rsvd1;
	uint64_t rsvd2;
};

struct drm_i915_gem_execbuffer2 {
	uint64_t buffers_ptr;   /* drm_i915_gem_exec_object2 array */
	uint32_t buffer_count;
	uint32_t batch_start_offset;
	uint32_t batch_len;
	uint32_t dr1, dr4;
	uint32_t num_cliprects;
	uint64_t cliprects_ptr;
	uint64_t flags;
	uint64_t rsvd1;         /* context id */
	uint64_t rsvd2;
};

struct drm_i915_gem_relocation_entry {
	uint64_t target_handle;     /* index into exec_object2 array (with LUT) */
	uint64_t delta;             /* added to target BO GTT offset */
	uint64_t offset;            /* byte offset in THIS BO to patch */
	uint64_t presumed_offset;   /* last known GTT offset of target */
	uint32_t read_domains;
	uint32_t write_domain;
};

/* exec_object2 flags */
#define EXEC_OBJECT_WRITE           (1 << 2)
#define EXEC_OBJECT_SUPPORTS_48B    (1 << 3)

struct drm_i915_gem_context_create {
	uint32_t ctx_id;    /* out */
	uint32_t pad;
};

struct drm_i915_gem_context_destroy {
	uint32_t ctx_id;
	uint32_t pad;
};

struct drm_i915_gem_set_tiling {
	uint32_t handle;
	uint32_t tiling_mode;    /* I915_TILING_NONE/X/Y */
	uint32_t stride;         /* row pitch in bytes */
	uint32_t swizzle_mode;   /* out: swizzle applied */
};

struct drm_i915_gem_get_tiling {
	uint32_t handle;
	uint32_t tiling_mode;    /* out */
	uint32_t swizzle_mode;   /* out */
	uint32_t phys_swizzle_mode; /* out */
};

struct drm_i915_gem_wait {
	uint32_t bo_handle;
	uint32_t flags;
	int64_t  timeout_ns;     /* in/out */
};

struct drm_i915_gem_context_param {
	uint32_t ctx_id;
	uint32_t size;
	uint64_t param;
	uint64_t value;
};

struct drm_i915_reset_stats {
	uint32_t ctx_id;
	uint32_t flags;
	uint32_t reset_count;
	uint32_t batch_active;
	uint32_t batch_pending;
	uint32_t pad;
};

/* Tiling modes */
#define I915_TILING_NONE    0
#define I915_TILING_X       1
#define I915_TILING_Y       2

/* Context params */
#define I915_CONTEXT_PARAM_GTT_SIZE   0x3

/* I915_PARAM values used by crocus */
#define I915_PARAM_CHIPSET_ID           4
#define I915_PARAM_HAS_EXECBUF2         9
#define I915_PARAM_NUM_FENCES_AVAIL     12
#define I915_PARAM_HAS_RELAXED_FENCING  23
#define I915_PARAM_MMAP_GTT_VERSION     40
#define I915_PARAM_REVISION             32
#define I915_PARAM_CS_TIMESTAMP_FREQUENCY 51

/* Exec flags */
#define I915_EXEC_RENDER        (1 << 0)
#define I915_EXEC_NO_RELOC      (1 << 11)
#define I915_EXEC_BATCH_FIRST   (1 << 18)
#define I915_EXEC_HANDLE_LUT    (1 << 12)

/* Domain flags */
#define I915_GEM_DOMAIN_CPU     0x1
#define I915_GEM_DOMAIN_RENDER  0x2
#define I915_GEM_DOMAIN_GTT     0x4

/* ---- Haiku DRM shim API ---- */

/* Initialize the shim. Opens the intel_extreme device.
 * Returns fd >= 0 on success. */
int haiku_drm_open(void);

/* Close the shim and free all resources. */
void haiku_drm_close(int fd);

/* Main ioctl dispatcher — call this instead of ioctl() for DRM ops. */
int haiku_drm_ioctl(int fd, unsigned long request, void* arg);

#endif /* HAIKU_DRM_INTEL_H */
