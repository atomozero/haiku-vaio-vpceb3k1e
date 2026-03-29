/*
 * drm_test - Test the libdrm_intel shim on Haiku
 *
 * Uses the standard drm_intel_* API (as Mesa crocus would)
 * to allocate buffers, build a BLT batch, and execute on the GPU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "drm_intel_shim.h"
#include "AccelerantIntel.h"


/* Gen5 BLT commands (from intel_extreme.h) */
#define XY_COMMAND_COLOR_BLIT	0x54000004
#define COMMAND_BLIT_RGBA		0x00300000
#define COMMAND_MODE_RGB32		0x03
#define MI_NOOP					0x00000000


static int
open_intel_device()
{
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL)
		return -1;

	char path[256] = {};
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path),
				"/dev/graphics/%s", entry->d_name);
			break;
		}
	}
	closedir(dir);

	if (path[0] == '\0')
		return -1;

	return open(path, O_RDWR);
}


int
main()
{
	printf("=== libdrm_intel Shim Test ===\n\n");

	int fd = open_intel_device();
	if (fd < 0) {
		printf("[FAIL] No intel_extreme device\n");
		return 1;
	}

	/* Initialize buffer manager */
	printf("--- bufmgr init ---\n");
	drm_intel_bufmgr* bufmgr = drm_intel_bufmgr_gem_init(fd, 4096);
	if (bufmgr == NULL) {
		printf("[FAIL] bufmgr init failed\n");
		close(fd);
		return 1;
	}
	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	int devid = drm_intel_bufmgr_gem_get_devid(bufmgr);
	int32_t fbOffset = 0, fbPitch = 0;
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_OFFSET, &fbOffset);
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_PITCH, &fbPitch);
	printf("  devid=0x%04x fb_offset=0x%x fb_pitch=%d\n",
		devid, fbOffset, fbPitch);

	/* Allocate destination buffer (64x64 @ 32bpp = 16KB) */
	printf("\n--- alloc ---\n");
	drm_intel_bo* dst = drm_intel_bo_alloc(bufmgr, "dst_surface",
		64 * 64 * 4, 4096);
	if (dst == NULL) {
		printf("[FAIL] dst alloc\n");
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}

	drm_intel_bo* batch = drm_intel_bo_alloc(bufmgr, "batch", 4096, 4096);
	if (batch == NULL) {
		printf("[FAIL] batch alloc\n");
		drm_intel_bo_unreference(dst);
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}
	printf("  dst: handle=%d offset=0x%lx\n", dst->handle, dst->offset);
	printf("  batch: handle=%d offset=0x%lx\n", batch->handle, batch->offset);

	/* Build BLT batch: fill 32x32 rect with red */
	printf("\n--- build batch ---\n");
	drm_intel_bo_map(batch, 1);
	uint32_t* cmds = (uint32_t*)batch->virtual_ptr;
	int i = 0;

	uint32_t pitch = 64 * 4;
	cmds[i++] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
	cmds[i++] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
		| (pitch & 0xFFFF);
	cmds[i++] = (0 << 16) | 0;				/* y1=0, x1=0 */
	cmds[i++] = (32 << 16) | 32;			/* y2=32, x2=32 */
	cmds[i++] = (uint32_t)dst->offset;		/* dst GPU address */
	cmds[i++] = 0xFFFF0000;				/* color: red */

	int batch_bytes = i * sizeof(uint32_t);
	printf("  %d dwords, dst=0x%lx\n", i, dst->offset);

	/* Execute */
	printf("\n--- exec ---\n");
	int ret = drm_intel_bo_exec(batch, batch_bytes, NULL, 0, 0);
	printf("  exec: %s\n", ret == 0 ? "OK" : "FAIL");

	if (ret != 0) {
		printf("  GPU may be hung — check ESR/ring status\n");
		drm_intel_bo_unreference(batch);
		drm_intel_bo_unreference(dst);
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}

	/* Wait */
	drm_intel_bo_wait_rendering(batch);
	printf("  wait: OK\n");

	/* Verify */
	printf("\n--- verify ---\n");
	drm_intel_bo_map(dst, 0);
	uint32_t* pixels = (uint32_t*)dst->virtual_ptr;

	if (pixels != NULL) {
		/* Flush CPU cache (GPU wrote via GTT, CPU may have stale data) */
		__asm__ __volatile__("mfence" ::: "memory");
		for (unsigned int cl = 0; cl < dst->size; cl += 64)
			__asm__ __volatile__("clflush (%0)" ::
				"r"((uint8_t*)pixels + cl));
		__asm__ __volatile__("mfence" ::: "memory");

		uint32_t p00 = pixels[0];
		uint32_t p1616 = pixels[16 * 64 + 16];
		uint32_t p4848 = pixels[48 * 64 + 48];

		printf("  pixel(0,0):   0x%08x %s\n", p00,
			p00 == 0xFFFF0000 ? "(red OK)" : "(check)");
		printf("  pixel(16,16): 0x%08x %s\n", p1616,
			p1616 == 0xFFFF0000 ? "(red OK)" : "(check)");
		printf("  pixel(48,48): 0x%08x %s\n", p4848,
			p4848 == 0 ? "(zero OK)" : "(check)");

		bool pass = (p00 == 0xFFFF0000 && p1616 == 0xFFFF0000
			&& p4848 == 0);
		printf("\n  %s\n", pass ? "[PASS] BLT fill verified!" :
			"[CHECK] Values differ from expected");
	}

	/* Cleanup */
	drm_intel_bo_unreference(batch);
	drm_intel_bo_unreference(dst);
	drm_intel_bufmgr_destroy(bufmgr);

	printf("\n=== Done ===\n");
	return 0;
}
