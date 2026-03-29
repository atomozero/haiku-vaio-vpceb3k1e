/*
 * drm_test - Test the libdrm_intel shim on Haiku
 *
 * Uses the standard drm_intel_* API (as Mesa crocus would)
 * to allocate buffers, build a batch, and execute GPU commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "drm_intel_shim.h"
#include "AccelerantIntel.h"	/* I915_PARAM_* constants */


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

	/* Open device */
	int fd = open_intel_device();
	if (fd < 0) {
		printf("[FAIL] No intel_extreme device\n");
		return 1;
	}

	/* Initialize buffer manager (like Mesa does at startup) */
	printf("--- bufmgr init ---\n");
	drm_intel_bufmgr* bufmgr = drm_intel_bufmgr_gem_init(fd, 4096);
	if (bufmgr == NULL) {
		printf("[FAIL] bufmgr init failed\n");
		close(fd);
		return 1;
	}

	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	int devid = drm_intel_bufmgr_gem_get_devid(bufmgr);
	printf("  devid: 0x%04x\n", devid);

	int32_t fbOffset = 0, fbPitch = 0;
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_OFFSET, &fbOffset);
	drm_intel_shim_get_param(bufmgr, I915_PARAM_FB_PITCH, &fbPitch);
	printf("  fb_offset: 0x%x, fb_pitch: %d\n", fbOffset, fbPitch);

	/* Allocate a destination buffer (64x64 @ 32bpp = 16KB) */
	printf("\n--- buffer alloc ---\n");
	drm_intel_bo* dst = drm_intel_bo_alloc(bufmgr, "dst_surface",
		64 * 64 * 4, 4096);
	if (dst == NULL) {
		printf("[FAIL] dst alloc failed\n");
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}
	printf("  dst: handle=%d offset=0x%lx size=%u\n",
		dst->handle, dst->offset, dst->size);

	/* Allocate a batch buffer */
	drm_intel_bo* batch = drm_intel_bo_alloc(bufmgr, "batch",
		4096, 4096);
	if (batch == NULL) {
		printf("[FAIL] batch alloc failed\n");
		drm_intel_bo_unreference(dst);
		drm_intel_bufmgr_destroy(bufmgr);
		return 1;
	}
	printf("  batch: handle=%d offset=0x%lx\n",
		batch->handle, batch->offset);

	/* Map batch buffer and build commands */
	printf("\n--- build batch ---\n");
	drm_intel_bo_map(batch, 1);

	uint32_t* cmds = (uint32_t*)batch->virtual_ptr;
	int i = 0;

	/* XY_COLOR_BLT: fill 32x32 rect at (0,0) with red
	 * DW0: opcode + RGBA write enable
	 * DW1: (mode << 24) | (rop << 16) | pitch
	 * DW2: (y1 << 16) | x1
	 * DW3: (y2 << 16) | x2
	 * DW4: dst GPU offset
	 * DW5: color (ARGB)
	 */
	/* First: BLT fill to dst buffer */
	uint32_t pitch = 64 * 4;		/* 64 pixels * 4 bytes */
	cmds[i++] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;
	cmds[i++] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
		| (pitch & 0xFFFF);
	cmds[i++] = (0 << 16) | 0;		/* y1=0, x1=0 */
	cmds[i++] = (32 << 16) | 32;	/* y2=32, x2=32 */
	cmds[i++] = (uint32_t)dst->offset;	/* dst GPU address */
	cmds[i++] = 0xFFFF0000;		/* color: red (ARGB) */

	printf("  BLT to dst offset 0x%lx (batch offset 0x%lx)\n",
		dst->offset, batch->offset);

	int batch_bytes = i * sizeof(uint32_t);

	printf("  Commands: %d dwords (%d bytes)\n", i, batch_bytes);
	printf("  dst offset: 0x%lx\n", dst->offset);

	/* Execute */
	printf("\n--- exec ---\n");
	int ret = drm_intel_bo_exec(batch, batch_bytes, NULL, 0, 0);
	printf("  exec: %s\n", ret == 0 ? "OK" : "FAIL");

	/* Wait for completion */
	drm_intel_bo_wait_rendering(batch);
	printf("  wait: OK\n");

	/* Debug: dump ring state and commands */
	printf("\n--- debug ---\n");
	printf("  batch->virtual_ptr=%p\n", batch->virtual_ptr);
	printf("  dst->virtual_ptr=%p\n", dst->virtual_ptr);
	printf("  batch cmds sent:\n");
	uint32_t* dbg = (uint32_t*)batch->virtual_ptr;
	for (int d = 0; d < 6; d++)
		printf("    [%d] 0x%08x\n", d, dbg[d]);

	/* CPU write test: write a marker to dst, read it back */
	drm_intel_bo_map(dst, 1);
	uint32_t* test_px = (uint32_t*)dst->virtual_ptr;
	test_px[63 * 64 + 63] = 0xDEADBEEF;
	printf("  CPU write test: pixel(63,63) = 0x%08x %s\n",
		test_px[63 * 64 + 63],
		test_px[63 * 64 + 63] == 0xDEADBEEF ? "OK" : "FAIL");

	/* Flush CPU caches — GPU wrote via GTT, CPU might have stale
	 * data in its cache. clflush each cache line of the dst buffer. */
	__asm__ __volatile__("mfence" ::: "memory");
	uint8_t* flush_ptr = (uint8_t*)dst->virtual_ptr;
	for (unsigned int cl = 0; cl < dst->size; cl += 64)
		__asm__ __volatile__("clflush (%0)" :: "r"(flush_ptr + cl));
	__asm__ __volatile__("mfence" ::: "memory");

	/* Verify: read back dst buffer */
	printf("\n--- verify ---\n");
	uint32_t* pixels = (uint32_t*)dst->virtual_ptr;

	if (pixels != NULL) {
		/* Check pixel at (0,0) — should be red */
		uint32_t p00 = pixels[0];
		/* Check pixel at (16,16) — should be red */
		uint32_t p1616 = pixels[16 * 64 + 16];
		/* Check pixel at (48,48) — outside fill, should be 0 */
		uint32_t p4848 = pixels[48 * 64 + 48];

		printf("  pixel(0,0):   0x%08x %s\n", p00,
			p00 == 0xFFFF0000 ? "(red OK)" : "(unexpected)");
		printf("  pixel(16,16): 0x%08x %s\n", p1616,
			p1616 == 0xFFFF0000 ? "(red OK)" : "(unexpected)");
		printf("  pixel(48,48): 0x%08x %s\n", p4848,
			p4848 == 0 ? "(zero OK)" : "(unexpected)");
	} else {
		printf("  [WARN] dst not mapped\n");
	}

	/* Cleanup */
	printf("\n--- cleanup ---\n");
	drm_intel_bo_unreference(batch);
	drm_intel_bo_unreference(dst);
	drm_intel_bufmgr_destroy(bufmgr);
	printf("  Done\n");

	printf("\n=== Shim test complete ===\n");
	return 0;
}
