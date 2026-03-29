/*
 * drm_intel_shim.h - libdrm_intel API shim for Haiku
 *
 * Provides the drm_intel_* API surface that Mesa crocus expects,
 * backed by our accelerant_i915 vtable instead of Linux DRM ioctls.
 *
 * Only the subset used by crocus Gen5 is implemented.
 */
#ifndef DRM_INTEL_SHIM_H
#define DRM_INTEL_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ---------------------------------------------------------------
 * Public structs (must match libdrm layout that Mesa expects)
 * --------------------------------------------------------------- */

struct drm_intel_bufmgr;

struct drm_intel_bo {
	unsigned int	size;
	unsigned int	align;
	unsigned long	offset;			/* GPU virtual address */
	uint64_t		offset64;		/* GPU virtual address (64-bit) */
	void*			virtual_ptr;	/* CPU mmap pointer (set after map) */
	int				handle;			/* GEM handle */
	struct drm_intel_bufmgr* bufmgr;
	const char*		name;
};

struct drm_intel_context {
	unsigned int	ctx_id;
	struct drm_intel_bufmgr* bufmgr;
};

/* Clip rect (unused on Gen5 but needed for API compat) */
struct drm_clip_rect {
	unsigned short	x1, y1, x2, y2;
};


/* ---------------------------------------------------------------
 * Buffer manager
 * --------------------------------------------------------------- */

struct drm_intel_bufmgr*
drm_intel_bufmgr_gem_init(int fd, int batch_size);

void
drm_intel_bufmgr_destroy(struct drm_intel_bufmgr* bufmgr);

void
drm_intel_bufmgr_gem_enable_reuse(struct drm_intel_bufmgr* bufmgr);

void
drm_intel_bufmgr_gem_enable_fenced_relocs(struct drm_intel_bufmgr* bufmgr);

int
drm_intel_bufmgr_gem_get_devid(struct drm_intel_bufmgr* bufmgr);


/* ---------------------------------------------------------------
 * Buffer objects
 * --------------------------------------------------------------- */

struct drm_intel_bo*
drm_intel_bo_alloc(struct drm_intel_bufmgr* bufmgr, const char* name,
	unsigned long size, unsigned int alignment);

void
drm_intel_bo_reference(struct drm_intel_bo* bo);

void
drm_intel_bo_unreference(struct drm_intel_bo* bo);

#define drm_intel_bo_unref(bo) drm_intel_bo_unreference(bo)

int
drm_intel_bo_map(struct drm_intel_bo* bo, int write_enable);

int
drm_intel_bo_unmap(struct drm_intel_bo* bo);

int
drm_intel_gem_bo_map_gtt(struct drm_intel_bo* bo);

int
drm_intel_bo_subdata(struct drm_intel_bo* bo, unsigned long offset,
	unsigned long size, const void* data);

int
drm_intel_bo_get_subdata(struct drm_intel_bo* bo, unsigned long offset,
	unsigned long size, void* data);

void
drm_intel_bo_wait_rendering(struct drm_intel_bo* bo);

int
drm_intel_bo_busy(struct drm_intel_bo* bo);


/* ---------------------------------------------------------------
 * Execution
 * --------------------------------------------------------------- */

int
drm_intel_bo_exec(struct drm_intel_bo* bo, int used,
	struct drm_clip_rect* cliprects, int num_cliprects, int DR4);

int
drm_intel_bo_emit_reloc(struct drm_intel_bo* bo, uint32_t offset,
	struct drm_intel_bo* target_bo, uint32_t target_offset,
	uint32_t read_domains, uint32_t write_domain);


/* ---------------------------------------------------------------
 * Tiling (stubs for Gen5 — linear only for now)
 * --------------------------------------------------------------- */

int
drm_intel_bo_set_tiling(struct drm_intel_bo* bo, uint32_t* tiling_mode,
	uint32_t stride);

int
drm_intel_bo_get_tiling(struct drm_intel_bo* bo, uint32_t* tiling_mode,
	uint32_t* swizzle_mode);


/* ---------------------------------------------------------------
 * Context (stubs)
 * --------------------------------------------------------------- */

struct drm_intel_context*
drm_intel_gem_context_create(struct drm_intel_bufmgr* bufmgr);

void
drm_intel_gem_context_destroy(struct drm_intel_context* ctx);

int
drm_intel_gem_bo_context_exec(struct drm_intel_bo* bo,
	struct drm_intel_context* ctx, int used, unsigned int flags);


/* Query GPU parameters (I915_PARAM_*) through the shim */
int
drm_intel_shim_get_param(struct drm_intel_bufmgr* bufmgr, uint32_t param,
	int32_t* value);


#ifdef __cplusplus
}
#endif

#endif	/* DRM_INTEL_SHIM_H */
