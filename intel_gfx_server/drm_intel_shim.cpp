/*
 * drm_intel_shim.cpp - libdrm_intel implementation for Haiku
 *
 * Translates drm_intel_* calls into accelerant_i915 vtable calls.
 * Built as libdrm_intel.so, drop-in replacement for Mesa crocus.
 */

#include "drm_intel_shim.h"
#include "AccelerantIntel.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


/* ---------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------- */

struct drm_intel_bufmgr {
	void*				addon;		/* dlopen handle */
	accelerant_i915*	i915;		/* GPU interface vtable */
	int					fd;
	int					devid;
	int					refcount;

	// For the C query helper
	typedef void* (*query_func)(void*, const char*, uint32);
	query_func			query_iface;
	void*				acc_raw;	/* opaque accelerant object */
};


/* Extended BO with refcount */
struct drm_intel_bo_priv {
	struct drm_intel_bo	pub;	/* public part (must be first) */
	int					refcount;
};

#define BO_PRIV(bo) ((drm_intel_bo_priv*)(bo))


/* ---------------------------------------------------------------
 * Buffer manager
 * --------------------------------------------------------------- */

struct drm_intel_bufmgr*
drm_intel_bufmgr_gem_init(int fd, int batch_size)
{
	(void)batch_size;

	drm_intel_bufmgr* mgr = (drm_intel_bufmgr*)calloc(1,
		sizeof(drm_intel_bufmgr));
	if (mgr == NULL)
		return NULL;

	mgr->fd = fd;
	mgr->refcount = 1;

	/* Load the accelerant2 add-on */
	const char* addon_path = getenv("INTEL_ACCELERANT2");
	if (addon_path == NULL)
		addon_path = "/boot/system/non-packaged/add-ons/accelerants/"
			"intel_gfx.accelerant2";

	mgr->addon = dlopen(addon_path, RTLD_LAZY);
	if (mgr->addon == NULL) {
		fprintf(stderr, "drm_intel_shim: dlopen(%s): %s\n",
			addon_path, dlerror());
		free(mgr);
		return NULL;
	}

	/* Instantiate */
	typedef status_t (*inst_func)(void**, int);
	inst_func instantiate = (inst_func)
		dlsym(mgr->addon, "instantiate_accelerant");
	if (instantiate == NULL) {
		fprintf(stderr, "drm_intel_shim: no instantiate_accelerant\n");
		dlclose(mgr->addon);
		free(mgr);
		return NULL;
	}

	status_t err = instantiate(&mgr->acc_raw, fd);
	if (err != B_OK) {
		fprintf(stderr, "drm_intel_shim: instantiate failed: %s\n",
			strerror(err));
		dlclose(mgr->addon);
		free(mgr);
		return NULL;
	}

	/* Get i915 interface via C helper */
	mgr->query_iface = (drm_intel_bufmgr::query_func)
		dlsym(mgr->addon, "accelerant_query_interface");
	if (mgr->query_iface == NULL) {
		fprintf(stderr, "drm_intel_shim: no query helper\n");
		dlclose(mgr->addon);
		free(mgr);
		return NULL;
	}

	mgr->i915 = (accelerant_i915*)
		mgr->query_iface(mgr->acc_raw, "i915/v1", 0);
	if (mgr->i915 == NULL) {
		fprintf(stderr, "drm_intel_shim: no i915/v1 interface\n");
		dlclose(mgr->addon);
		free(mgr);
		return NULL;
	}

	/* Cache device ID */
	int32 chipId = 0;
	mgr->i915->vt->GetParam(mgr->i915, I915_PARAM_CHIPSET_ID, &chipId);
	mgr->devid = chipId;

	printf("drm_intel_shim: ready (devid=0x%04x)\n", mgr->devid);
	return mgr;
}


void
drm_intel_bufmgr_destroy(struct drm_intel_bufmgr* bufmgr)
{
	if (bufmgr == NULL)
		return;
	if (bufmgr->addon != NULL)
		dlclose(bufmgr->addon);
	free(bufmgr);
}


void
drm_intel_bufmgr_gem_enable_reuse(struct drm_intel_bufmgr* bufmgr)
{
	/* No-op: our GemManager doesn't pool buffers yet */
	(void)bufmgr;
}


void
drm_intel_bufmgr_gem_enable_fenced_relocs(struct drm_intel_bufmgr* bufmgr)
{
	(void)bufmgr;
}


int
drm_intel_bufmgr_gem_get_devid(struct drm_intel_bufmgr* bufmgr)
{
	return bufmgr->devid;
}


/* ---------------------------------------------------------------
 * Buffer objects
 * --------------------------------------------------------------- */

struct drm_intel_bo*
drm_intel_bo_alloc(struct drm_intel_bufmgr* bufmgr, const char* name,
	unsigned long size, unsigned int alignment)
{
	(void)alignment;	/* GemManager aligns to page internally */

	drm_intel_bo_priv* priv = (drm_intel_bo_priv*)calloc(1,
		sizeof(drm_intel_bo_priv));
	if (priv == NULL)
		return NULL;

	uint32 handle = 0;
	int ret = bufmgr->i915->vt->GemCreate(bufmgr->i915,
		(uint32)size, &handle);
	if (ret != 0) {
		free(priv);
		return NULL;
	}

	uint32 offset = bufmgr->i915->vt->GemGetOffset(bufmgr->i915, handle);

	priv->pub.size = (unsigned int)size;
	priv->pub.align = alignment;
	priv->pub.offset = offset;
	priv->pub.offset64 = offset;
	priv->pub.virtual_ptr = NULL;
	priv->pub.handle = handle;
	priv->pub.bufmgr = bufmgr;
	priv->pub.name = name;
	priv->refcount = 1;

	return &priv->pub;
}


void
drm_intel_bo_reference(struct drm_intel_bo* bo)
{
	if (bo != NULL)
		BO_PRIV(bo)->refcount++;
}


void
drm_intel_bo_unreference(struct drm_intel_bo* bo)
{
	if (bo == NULL)
		return;

	drm_intel_bo_priv* priv = BO_PRIV(bo);
	if (--priv->refcount > 0)
		return;

	bo->bufmgr->i915->vt->GemClose(bo->bufmgr->i915, bo->handle);
	free(priv);
}


int
drm_intel_bo_map(struct drm_intel_bo* bo, int write_enable)
{
	(void)write_enable;

	if (bo->virtual_ptr != NULL)
		return 0;	/* already mapped */

	void* ptr = bo->bufmgr->i915->vt->GemMmap(bo->bufmgr->i915,
		bo->handle);
	if (ptr == NULL)
		return -1;

	bo->virtual_ptr = ptr;
	return 0;
}


int
drm_intel_bo_unmap(struct drm_intel_bo* bo)
{
	/* GTT-mapped buffers stay mapped until close */
	(void)bo;
	return 0;
}


int
drm_intel_gem_bo_map_gtt(struct drm_intel_bo* bo)
{
	return drm_intel_bo_map(bo, 1);
}


int
drm_intel_bo_subdata(struct drm_intel_bo* bo, unsigned long offset,
	unsigned long size, const void* data)
{
	int ret = drm_intel_bo_map(bo, 1);
	if (ret != 0)
		return ret;

	memcpy((uint8*)bo->virtual_ptr + offset, data, size);
	return 0;
}


int
drm_intel_bo_get_subdata(struct drm_intel_bo* bo, unsigned long offset,
	unsigned long size, void* data)
{
	int ret = drm_intel_bo_map(bo, 0);
	if (ret != 0)
		return ret;

	memcpy(data, (uint8*)bo->virtual_ptr + offset, size);
	return 0;
}


void
drm_intel_bo_wait_rendering(struct drm_intel_bo* bo)
{
	bo->bufmgr->i915->vt->WaitIdle(bo->bufmgr->i915, 1000000);
}


int
drm_intel_bo_busy(struct drm_intel_bo* bo)
{
	(void)bo;
	return 0;	/* TODO: check ring idle status */
}


/* ---------------------------------------------------------------
 * Execution
 * --------------------------------------------------------------- */

int
drm_intel_bo_exec(struct drm_intel_bo* bo, int used,
	struct drm_clip_rect* cliprects, int num_cliprects, int DR4)
{
	(void)cliprects;
	(void)num_cliprects;
	(void)DR4;

	if (bo->virtual_ptr == NULL)
		return -1;

	/* Submit the batch buffer contents as ring commands.
	 * 'used' is the number of bytes used in the batch buffer. */
	uint32 count = used / sizeof(uint32);
	return bo->bufmgr->i915->vt->ExecCommands(bo->bufmgr->i915,
		(const uint32*)bo->virtual_ptr, count);
}


int
drm_intel_bo_emit_reloc(struct drm_intel_bo* bo, uint32_t offset,
	struct drm_intel_bo* target_bo, uint32_t target_offset,
	uint32_t read_domains, uint32_t write_domain)
{
	(void)read_domains;
	(void)write_domain;

	/* Patch the relocation in-place: write target's GPU offset
	 * into the batch buffer at the specified offset.
	 * This works because our GTT offsets are fixed (no relocation
	 * tables needed — buffers don't move). */
	if (bo->virtual_ptr == NULL) {
		int ret = drm_intel_bo_map(bo, 1);
		if (ret != 0)
			return ret;
	}

	uint32_t* reloc_ptr = (uint32_t*)((uint8*)bo->virtual_ptr + offset);
	*reloc_ptr = (uint32_t)(target_bo->offset + target_offset);

	return 0;
}


/* ---------------------------------------------------------------
 * Tiling stubs (linear only for now)
 * --------------------------------------------------------------- */

int
drm_intel_bo_set_tiling(struct drm_intel_bo* bo, uint32_t* tiling_mode,
	uint32_t stride)
{
	(void)bo;
	(void)stride;
	if (tiling_mode != NULL)
		*tiling_mode = 0;	/* I915_TILING_NONE */
	return 0;
}


int
drm_intel_bo_get_tiling(struct drm_intel_bo* bo, uint32_t* tiling_mode,
	uint32_t* swizzle_mode)
{
	(void)bo;
	if (tiling_mode != NULL)
		*tiling_mode = 0;
	if (swizzle_mode != NULL)
		*swizzle_mode = 0;
	return 0;
}


/* ---------------------------------------------------------------
 * Context stubs
 * --------------------------------------------------------------- */

struct drm_intel_context*
drm_intel_gem_context_create(struct drm_intel_bufmgr* bufmgr)
{
	drm_intel_context* ctx = (drm_intel_context*)calloc(1,
		sizeof(drm_intel_context));
	if (ctx != NULL) {
		ctx->ctx_id = 0;	/* single context */
		ctx->bufmgr = bufmgr;
	}
	return ctx;
}


void
drm_intel_gem_context_destroy(struct drm_intel_context* ctx)
{
	free(ctx);
}


int
drm_intel_gem_bo_context_exec(struct drm_intel_bo* bo,
	struct drm_intel_context* ctx, int used, unsigned int flags)
{
	(void)ctx;
	(void)flags;
	return drm_intel_bo_exec(bo, used, NULL, 0, 0);
}


/* ---------------------------------------------------------------
 * Shim-specific helpers
 * --------------------------------------------------------------- */

int
drm_intel_shim_get_param(struct drm_intel_bufmgr* bufmgr, uint32_t param,
	int32_t* value)
{
	return bufmgr->i915->vt->GetParam(bufmgr->i915, param, (int32*)value);
}
