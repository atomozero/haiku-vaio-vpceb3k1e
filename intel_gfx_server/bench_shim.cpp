/*
 * bench_shim - BLT benchmark through drm_intel shim
 *
 * Compares:
 * 1. BLT via shim (with MI_FLUSH overhead)
 * 2. Single BLT latency via shim
 *
 * Run alongside bench_gpu for comparison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <OS.h>

#include "drm_intel_shim.h"
#include "AccelerantIntel.h"


#define XY_COMMAND_COLOR_BLIT	0x54000004
#define COMMAND_BLIT_RGBA		0x00300000
#define COMMAND_MODE_RGB32		0x03


static int
open_intel_device()
{
	DIR* dir = opendir("/dev/graphics");
	if (!dir) return -1;
	char path[256] = {};
	struct dirent* e;
	while ((e = readdir(dir)))
		if (strncmp(e->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name);
			break;
		}
	closedir(dir);
	if (!path[0]) return -1;
	return open(path, O_RDWR);
}


struct bench_size {
	uint32	w, h;
	const char* label;
};

static const bench_size kSizes[] = {
	{   8,    8, "8x8" },
	{  32,   32, "32x32" },
	{  64,   64, "64x64" },
	{ 256,  256, "256x256" },
	{ 512,  512, "512x512" },
	{1024,  768, "1024x768" },
	{1366,  768, "1366x768" },
};
static const int kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
static const bigtime_t kBenchDuration = 2000000;	// 2 seconds per size


int
main()
{
	printf("=== BLT Benchmark via drm_intel Shim ===\n\n");

	int fd = open_intel_device();
	if (fd < 0) {
		printf("[FAIL] No device\n");
		return 1;
	}

	drm_intel_bufmgr* bufmgr = drm_intel_bufmgr_gem_init(fd, 4096);
	if (!bufmgr) {
		printf("[FAIL] bufmgr init\n");
		return 1;
	}

	int32_t fbOffset = 0, fbPitch = 0;
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_OFFSET, &fbOffset);
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_PITCH, &fbPitch);
	printf("  fb_offset=0x%x pitch=%d\n\n", fbOffset, fbPitch);

	// Allocate a batch buffer (reused for all tests)
	drm_intel_bo* batch = drm_intel_bo_alloc(bufmgr, "batch", 4096, 4096);
	if (!batch) {
		printf("[FAIL] batch alloc\n");
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}
	drm_intel_bo_map(batch, 1);

	// ---------------------------------------------------------------
	// Test 1: BLT fill throughput (single BLT per exec call)
	// ---------------------------------------------------------------
	printf("--- BLT fill (1 per exec, with MI_FLUSH) ---\n");
	printf("  %-12s %8s %10s\n", "Size", "Iters", "MB/s");

	for (int s = 0; s < kNumSizes; s++) {
		uint32 w = kSizes[s].w;
		uint32 h = kSizes[s].h;
		int totalIters = 0;
		bool ok = true;

		bigtime_t start = system_time();
		while (system_time() - start < kBenchDuration && ok) {
			uint32_t* cmds = (uint32_t*)batch->virtual_ptr;
			cmds[0] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
			cmds[1] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
				| (fbPitch & 0xFFFF);
			cmds[2] = 0;					// top-left
			cmds[3] = (h << 16) | w;		// bottom-right
			cmds[4] = fbOffset;
			cmds[5] = 0xFF000080 + (totalIters & 0x7F);

			if (drm_intel_bo_exec(batch, 24, NULL, 0, 0) != 0) {
				ok = false;
				break;
			}
			drm_intel_bo_wait_rendering(batch);
			totalIters++;
		}
		bigtime_t elapsed = system_time() - start;

		double totalMB = (double)w * h * 4.0 * totalIters / 1048576.0;
		double secs = (double)elapsed / 1000000.0;

		printf("  %-12s %8d %10.1f%s\n",
			kSizes[s].label, totalIters, totalMB / secs,
			ok ? "" : " ERR");
	}

	// ---------------------------------------------------------------
	// Test 2: BLT fill throughput (batched, N BLTs per exec)
	// ---------------------------------------------------------------
	printf("\n--- BLT fill (batched, N per exec, with MI_FLUSH) ---\n");
	printf("  %-12s %8s %10s\n", "Size", "Iters", "MB/s");

	// Re-alloc a bigger batch for batching
	drm_intel_bo_unreference(batch);
	batch = drm_intel_bo_alloc(bufmgr, "batch_big", 32768, 4096);
	drm_intel_bo_map(batch, 1);

	for (int s = 0; s < kNumSizes; s++) {
		uint32 w = kSizes[s].w;
		uint32 h = kSizes[s].h;
		uint32 pixels = w * h;
		int batchCount = (pixels > 100000) ? 20 : 500;
		int totalIters = 0;
		bool ok = true;

		bigtime_t start = system_time();
		while (system_time() - start < kBenchDuration && ok) {
			uint32_t* cmds = (uint32_t*)batch->virtual_ptr;
			int ci = 0;
			for (int b = 0; b < batchCount; b++) {
				cmds[ci++] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
				cmds[ci++] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
					| (fbPitch & 0xFFFF);
				cmds[ci++] = 0;
				cmds[ci++] = (h << 16) | w;
				cmds[ci++] = fbOffset;
				cmds[ci++] = 0xFF000080 + (totalIters & 0x7F);
			}

			if (drm_intel_bo_exec(batch, ci * 4, NULL, 0, 0) != 0) {
				ok = false;
				break;
			}
			drm_intel_bo_wait_rendering(batch);
			totalIters += batchCount;
		}
		bigtime_t elapsed = system_time() - start;

		double totalMB = (double)w * h * 4.0 * totalIters / 1048576.0;
		double secs = (double)elapsed / 1000000.0;

		printf("  %-12s %8d %10.1f%s\n",
			kSizes[s].label, totalIters, totalMB / secs,
			ok ? "" : " ERR");
	}

	// ---------------------------------------------------------------
	// Test 3: Single BLT round-trip latency
	// ---------------------------------------------------------------
	printf("\n--- Single BLT round-trip latency ---\n");
	drm_intel_bo_unreference(batch);
	batch = drm_intel_bo_alloc(bufmgr, "batch_lat", 4096, 4096);
	drm_intel_bo_map(batch, 1);

	const int kLatIters = 1000;
	bigtime_t latStart = system_time();
	for (int i = 0; i < kLatIters; i++) {
		uint32_t* cmds = (uint32_t*)batch->virtual_ptr;
		cmds[0] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
		cmds[1] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
			| (16 & 0xFFFF);
		cmds[2] = 0;
		cmds[3] = (4 << 16) | 4;
		cmds[4] = fbOffset;
		cmds[5] = 0xFF000080;
		drm_intel_bo_exec(batch, 24, NULL, 0, 0);
		drm_intel_bo_wait_rendering(batch);
	}
	bigtime_t latElapsed = system_time() - latStart;
	printf("  %d iters, avg %.1f us/iter\n",
		kLatIters, (double)latElapsed / kLatIters);

	drm_intel_bo_unreference(batch);
	drm_intel_bufmgr_destroy(bufmgr);

	printf("\n=== Done ===\n");
	return 0;
}
