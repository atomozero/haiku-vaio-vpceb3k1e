/*
 * AccelerantIntel - accelerant2 interface for Intel Gen5 on Haiku
 *
 * Implements a custom AccelerantI915 interface (buffer create,
 * command execution, sync) following X547's accelerant2 pattern.
 * Direct GPU access via shared_info (no IPC server for now).
 */
#pragma once

#include <OS.h>
#include <SupportDefs.h>

#include "GemManager.h"


// Interface identifiers (accelerant2 convention)
#define B_ACCELERANT_IFACE_BASE		"base/v1"
#define B_ACCELERANT_IFACE_DRM		"drm/v1"
#define B_ACCELERANT_IFACE_I915		"i915/v1"


// ---------------------------------------------------------------
// AccelerantI915 - Intel-specific GPU operations (C vtable)
// ---------------------------------------------------------------

struct accelerant_i915;

typedef struct accelerant_i915_vtable {
	// Buffer object management
	int (*GemCreate)(accelerant_i915* acc, uint32 size, uint32* handle);
	void* (*GemMmap)(accelerant_i915* acc, uint32 handle);
	uint32 (*GemGetOffset)(accelerant_i915* acc, uint32 handle);
	int (*GemClose)(accelerant_i915* acc, uint32 handle);

	// Command execution (copies into ring)
	int (*ExecCommands)(accelerant_i915* acc, const uint32* cmds,
		uint32 count);

	// Batch buffer execution (GPU reads from GEM buffer)
	int (*ExecBatch)(accelerant_i915* acc, uint32 handle,
		uint32 usedBytes);

	// Sync
	int (*WaitIdle)(accelerant_i915* acc, int64 timeout_us);

	// Query
	int (*GetParam)(accelerant_i915* acc, uint32 param, int32* value);
} accelerant_i915_vtable;

typedef struct accelerant_i915 {
	accelerant_i915_vtable*	vt;
} accelerant_i915;

// GetParam parameter IDs
#define I915_PARAM_CHIPSET_ID		1
#define I915_PARAM_HAS_GEM			2
#define I915_PARAM_NUM_FENCES		3
#define I915_PARAM_GTT_SIZE			4
#define I915_PARAM_GENERATION		5
#define I915_PARAM_FB_OFFSET		100
#define I915_PARAM_FB_PITCH			101


// ---------------------------------------------------------------
// C++ implementation class
// ---------------------------------------------------------------

#ifdef __cplusplus

class IntelAccelerant {
public:
						IntelAccelerant();
						~IntelAccelerant();

	status_t			Init(int deviceFd);

	// QueryInterface - returns pointer to requested interface
	void*				QueryInterface(const char* iface, uint32 version);

	// I915 operations (GEM + exec)
	int					GemCreate(uint32 size, uint32* handle);
	void*				GemMmap(uint32 handle);
	uint32				GemGetOffset(uint32 handle);
	int					GemClose(uint32 handle);
	int					ExecCommands(const uint32* cmds, uint32 count);
	int					ExecBatch(uint32 handle, uint32 usedBytes);
	int					WaitIdle(int64 timeout_us);
	int					GetParam(uint32 param, int32* value);

	// Public so C trampolines can use offsetof()
	accelerant_i915			fI915Iface;

private:
	int					fDeviceFd;
	area_id				fSharedInfoArea;
	intel_shared_info*	fSharedInfo;
	area_id				fRegsArea;
	volatile uint8*		fRegisters;

	GemManager			fGem;

	accelerant_i915_vtable	fI915Vtable;
};

#endif	// __cplusplus
