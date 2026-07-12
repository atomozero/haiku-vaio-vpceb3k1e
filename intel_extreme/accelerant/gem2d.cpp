/*
 * gem2d — 2D acceleration through the kernel GEM execbuffer path.
 * See gem2d.h for the model.
 */


#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "accelerant.h"
#include "intel_extreme.h"
#include "intel_gem.h"
#include "gem2d.h"


#undef TRACE
//#define TRACE_GEM2D
#ifdef TRACE_GEM2D
#	define TRACE(x...) _sPrintf("intel_extreme gem2d: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme gem2d: " x)


// Two batch BOs in round-robin: while the GPU executes one, the CPU can
// already fill the other. Reuse waits for retirement and moves the BO
// back to the CPU domain (the kernel skips the clflush otherwise).
static const int kBatchCount = 2;
static const size_t kBatchSize = 32 * 1024;

static bool sAvailable = false;
static uint32 sHandle[kBatchCount];
static area_id sArea[kBatchCount];
static uint8* sMap[kBatchCount];
static int sCurrent = 0;
static size_t sPosition = 0;
static bool sOpen = false;

// Perf-comparison toggle: when the file below exists, 2D reverts to the
// legacy ring path (pre-M4 behaviour) so the two submission mechanisms
// can be A/B benchmarked in a single boot. Re-checked at most every
// 200 ms to keep the hot path free of a stat() per drawing op.
static const char* kLegacyToggle =
	"/boot/home/config/settings/intel_2d_legacy";
static bigtime_t sToggleChecked = 0;
static bool sForceLegacy = false;

static bool
force_legacy()
{
	bigtime_t now = system_time();
	if (now - sToggleChecked > 200000) {
		sForceLegacy = access(kLegacyToggle, F_OK) == 0;
		sToggleChecked = now;
	}
	return sForceLegacy;
}


static status_t
gem_ioctl(uint32 op, void* buffer, size_t size)
{
	if (ioctl(gInfo->device, op, buffer, size) != 0)
		return errno;
	return B_OK;
}


status_t
gem2d_init()
{
	// Probe: the kernel reports GEM availability via INTEL_GET_PARAM.
	intel_gem_get_param param;
	param.magic = INTEL_PRIVATE_DATA_MAGIC;
	param.size = sizeof(param);
	param.param = INTEL_GEM_PARAM_HAS_GEM;
	param.value = 0;
	if (gem_ioctl(INTEL_GET_PARAM, &param, sizeof(param)) != B_OK
		|| param.value == 0) {
		TRACE("kernel GEM not available, keeping the legacy ring path\n");
		return B_NOT_SUPPORTED;
	}

	for (int i = 0; i < kBatchCount; i++) {
		intel_gem_create create;
		create.magic = INTEL_PRIVATE_DATA_MAGIC;
		create.size = sizeof(create);
		create.bo_size = kBatchSize;
		create.flags = 0;
		if (gem_ioctl(INTEL_GEM_CREATE, &create, sizeof(create)) != B_OK) {
			ERROR("batch BO create failed\n");
			gem2d_uninit();
			return B_ERROR;
		}
		sHandle[i] = create.handle;

		sArea[i] = clone_area("gem2d batch", (void**)&sMap[i],
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, create.area);
		if (sArea[i] < B_OK) {
			ERROR("batch BO clone failed\n");
			gem2d_uninit();
			return B_ERROR;
		}
	}

	sAvailable = true;
	_sPrintf("intel_extreme: 2D acceleration on the GEM execbuffer path "
		"(M4)\n");
	return B_OK;
}


void
gem2d_uninit()
{
	sAvailable = false;
	for (int i = 0; i < kBatchCount; i++) {
		if (sArea[i] > 0) {
			delete_area(sArea[i]);
			sArea[i] = -1;
			sMap[i] = NULL;
		}
		if (sHandle[i] != 0) {
			intel_gem_close close;
			close.magic = INTEL_PRIVATE_DATA_MAGIC;
			close.size = sizeof(close);
			close.handle = sHandle[i];
			gem_ioctl(INTEL_GEM_CLOSE, &close, sizeof(close));
			sHandle[i] = 0;
		}
	}
}


bool
gem2d_available()
{
	return sAvailable && !force_legacy();
}


static void
wait_batch(int index)
{
	intel_gem_wait wait;
	wait.magic = INTEL_PRIVATE_DATA_MAGIC;
	wait.size = sizeof(wait);
	wait.handle = sHandle[index];
	wait.timeout = B_INFINITE_TIMEOUT;
	gem_ioctl(INTEL_GEM_WAIT, &wait, sizeof(wait));
}


void
gem2d_begin()
{
	if (!sAvailable || sOpen)
		return;

	sCurrent = (sCurrent + 1) % kBatchCount;
	wait_batch(sCurrent);

	// Back to the CPU domain before rewriting: the kernel tracks the
	// domain and skips the clflush on submission otherwise.
	intel_gem_set_domain domain;
	domain.magic = INTEL_PRIVATE_DATA_MAGIC;
	domain.size = sizeof(domain);
	domain.handle = sHandle[sCurrent];
	domain.read_domains = INTEL_GEM_DOMAIN_CPU;
	domain.write_domain = INTEL_GEM_DOMAIN_CPU;
	gem_ioctl(INTEL_GEM_SET_DOMAIN, &domain, sizeof(domain));

	sPosition = 0;
	sOpen = true;
}


void
gem2d_end()
{
	if (!sAvailable || !sOpen)
		return;
	sOpen = false;

	if (sPosition == 0)
		return;

	uint32* tail = (uint32*)(sMap[sCurrent] + sPosition);
	*tail++ = MI_BATCH_BUFFER_END;
	size_t length = sPosition + sizeof(uint32);
	if (length & 0x07) {
		*tail++ = MI_NOOP;
		length += sizeof(uint32);
	}

	intel_gem_exec_object object;
	memset(&object, 0, sizeof(object));
	object.handle = sHandle[sCurrent];

	intel_gem_execbuffer exec;
	memset(&exec, 0, sizeof(exec));
	exec.magic = INTEL_PRIVATE_DATA_MAGIC;
	exec.size = sizeof(exec);
	exec.buffers_ptr = (uint64)(addr_t)&object;
	exec.buffer_count = 1;
	exec.batch_start_offset = 0;
	exec.batch_len = length;

	status_t status = gem_ioctl(INTEL_GEM_EXECBUFFER, &exec, sizeof(exec));
	if (status != B_OK) {
		// A failed submission (e.g. wedged engine) must not wedge the
		// desktop: drop 2D acceleration, app_server falls back to
		// software drawing via the framebuffer.
		ERROR("execbuffer failed (%s), disabling GEM 2D\n", strerror(status));
		sAvailable = false;
	}
}


void
gem2d_add(const void* commands, size_t size)
{
	if (!sAvailable)
		return;
	if (!sOpen)
		gem2d_begin();

	// Keep room for the MI_BATCH_BUFFER_END trailer
	if (sPosition + size + 16 > kBatchSize) {
		gem2d_end();
		gem2d_begin();
	}

	memcpy(sMap[sCurrent] + sPosition, commands, size);
	sPosition += size;
}


void
gem2d_wait_idle()
{
	if (!sAvailable)
		return;
	for (int i = 0; i < kBatchCount; i++)
		wait_batch(i);
}
