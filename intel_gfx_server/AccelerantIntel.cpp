/*
 * AccelerantIntel - accelerant2 implementation for Intel Gen5
 *
 * Provides GEM buffer management and command execution through
 * the i915/v1 interface. Uses GemManager for GPU access.
 */

#include "AccelerantIntel.h"
#include "accelerant.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


// ---------------------------------------------------------------
// C vtable trampolines
// ---------------------------------------------------------------

static IntelAccelerant*
_Self(accelerant_i915* acc)
{
	return (IntelAccelerant*)((uint8*)acc
		- offsetof(IntelAccelerant, fI915Iface));
}


static int
_GemCreate(accelerant_i915* a, uint32 size, uint32* h)
{
	return _Self(a)->GemCreate(size, h);
}

static void*
_GemMmap(accelerant_i915* a, uint32 h)
{
	return _Self(a)->GemMmap(h);
}

static uint32
_GemGetOffset(accelerant_i915* a, uint32 h)
{
	return _Self(a)->GemGetOffset(h);
}

static int
_GemClose(accelerant_i915* a, uint32 h)
{
	return _Self(a)->GemClose(h);
}

static int
_ExecCommands(accelerant_i915* a, const uint32* c, uint32 n)
{
	return _Self(a)->ExecCommands(c, n);
}

static int
_WaitIdle(accelerant_i915* a, int64 t)
{
	return _Self(a)->WaitIdle(t);
}

static int
_GetParam(accelerant_i915* a, uint32 p, int32* v)
{
	return _Self(a)->GetParam(p, v);
}


// ---------------------------------------------------------------
// Construction / initialization
// ---------------------------------------------------------------

IntelAccelerant::IntelAccelerant()
	:
	fDeviceFd(-1),
	fSharedInfoArea(-1),
	fSharedInfo(NULL),
	fRegsArea(-1),
	fRegisters(NULL)
{
	// Set up C vtable
	fI915Vtable.GemCreate = _GemCreate;
	fI915Vtable.GemMmap = _GemMmap;
	fI915Vtable.GemGetOffset = _GemGetOffset;
	fI915Vtable.GemClose = _GemClose;
	fI915Vtable.ExecCommands = _ExecCommands;
	fI915Vtable.WaitIdle = _WaitIdle;
	fI915Vtable.GetParam = _GetParam;

	fI915Iface.vt = &fI915Vtable;
}


IntelAccelerant::~IntelAccelerant()
{
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	if (fSharedInfoArea >= 0)
		delete_area(fSharedInfoArea);
	if (fDeviceFd >= 0)
		close(fDeviceFd);
}


status_t
IntelAccelerant::Init(int deviceFd)
{
	fDeviceFd = deviceFd;

	// Get shared_info from kernel driver via ioctl
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fDeviceFd, INTEL_GET_PRIVATE_DATA, &data,
			sizeof(intel_get_private_data)) != 0) {
		printf("AccelerantIntel: INTEL_GET_PRIVATE_DATA failed\n");
		return B_ERROR;
	}

	fSharedInfoArea = clone_area("accel2_intel shared_info",
		(void**)&fSharedInfo, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (fSharedInfoArea < 0) {
		printf("AccelerantIntel: cannot clone shared_info\n");
		return fSharedInfoArea;
	}

	fRegsArea = clone_area("accel2_intel registers",
		(void**)&fRegisters, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		fSharedInfo->registers_area);
	if (fRegsArea < 0) {
		printf("AccelerantIntel: cannot clone registers\n");
		return fRegsArea;
	}

	printf("AccelerantIntel: device 0x%04" B_PRIx32 " Gen%d\n",
		fSharedInfo->device_type.type,
		fSharedInfo->device_type.Generation());

	// Initialize GEM manager
	return fGem.Init(fSharedInfo, fRegisters, fDeviceFd);
}


// ---------------------------------------------------------------
// QueryInterface
// ---------------------------------------------------------------

void*
IntelAccelerant::QueryInterface(const char* iface, uint32 version)
{
	if (strcmp(iface, B_ACCELERANT_IFACE_I915) == 0)
		return &fI915Iface;

	return NULL;
}


// ---------------------------------------------------------------
// GEM operations
// ---------------------------------------------------------------

int
IntelAccelerant::GemCreate(uint32 size, uint32* handle)
{
	return fGem.CreateBuffer(size, handle) == B_OK ? 0 : -1;
}


void*
IntelAccelerant::GemMmap(uint32 handle)
{
	return fGem.MapBuffer(handle);
}


uint32
IntelAccelerant::GemGetOffset(uint32 handle)
{
	return fGem.GetOffset(handle);
}


int
IntelAccelerant::GemClose(uint32 handle)
{
	return fGem.CloseBuffer(handle) == B_OK ? 0 : -1;
}


int
IntelAccelerant::ExecCommands(const uint32* cmds, uint32 count)
{
	return fGem.ExecCommands(cmds, count) == B_OK ? 0 : -1;
}


int
IntelAccelerant::WaitIdle(int64 timeout_us)
{
	return fGem.WaitIdle(timeout_us) == B_OK ? 0 : -1;
}


int
IntelAccelerant::GetParam(uint32 param, int32* value)
{
	if (value == NULL)
		return -1;

	switch (param) {
		case I915_PARAM_CHIPSET_ID:
			*value = (int32)fSharedInfo->device_type.type;
			return 0;
		case I915_PARAM_HAS_GEM:
			*value = 1;
			return 0;
		case I915_PARAM_GTT_SIZE:
			*value = (int32)fSharedInfo->graphics_memory_size;
			return 0;
		case I915_PARAM_GENERATION:
			*value = fSharedInfo->device_type.Generation();
			return 0;
		case I915_PARAM_FB_OFFSET:
			*value = (int32)fSharedInfo->frame_buffer_offset;
			return 0;
		case I915_PARAM_FB_PITCH:
			*value = (int32)fSharedInfo->bytes_per_row;
			return 0;
		default:
			return -1;
	}
}
