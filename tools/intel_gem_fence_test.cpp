/*
 * intel_gem_fence_test — validates the Gen4/5 fence-register support in
 * the intel_extreme GEM kernel driver, in isolation (no display, no
 * crocus).
 *
 * A fence register makes the memory controller de-tile a tiled BO when
 * it is accessed LINEARLY through the mappable GTT aperture. The test:
 *   1. creates a BO, records X-tiling on it (native INTEL_GEM_SET_TILING),
 *      which programs a fence over its GTT range,
 *   2. maps its aperture window (DRM MMAP_WC) — the fenced, linear view,
 *   3. writes a known linear image through that window,
 *   4. reads it back through the window: must be identical (the hardware
 *      detile is the exact inverse of the tile-on-write),
 *   5. reads the raw BO bytes coherently (INTEL_GEM_READ_BO): must DIFFER
 *      from the linear image (memory is genuinely tiled), and must match
 *      the software X-tile+swizzle layout at spot-checked positions,
 *   6. repeats untiled as a control: raw == linear (no fence, no tiling).
 *
 * Build: g++ -Wall -O2 -o intel_gem_fence_test intel_gem_fence_test.cpp \
 *   -I<haiku-build>/headers/private/graphics/intel_extreme \
 *   -I<haiku-build>/src/add-ons/kernel/drivers/graphics/intel_extreme
 */

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <intel_extreme.h>
#include <intel_gem.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/i915_drm.h"


static const char* kDevice = "/dev/graphics/intel_extreme_000200";

static int sFd = -1;
static int sPass = 0;
static int sFail = 0;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  [PASS] %s\n", msg); sPass++; } \
	else { printf("  [FAIL] %s\n", msg); sFail++; } \
} while (0)


// Gen5 X-tile: 512-byte-wide, 8-row tiles (4096 bytes each). Plus the
// Ironlake bit-6 swizzle (bit 6 XOR bits 9 and 10). Maps a linear byte
// position to its physical offset in the tiled BO.
static uint32
x_tile_offset(uint32 col_byte, uint32 row, uint32 stride)
{
	uint32 tiles_per_row = stride / 512;
	uint32 tile_x = col_byte / 512;
	uint32 tile_y = row / 8;
	uint32 intra_x = col_byte % 512;
	uint32 intra_y = row % 8;
	uint32 off = (tile_y * tiles_per_row + tile_x) * 4096
		+ intra_y * 512 + intra_x;
	if ((((off >> 9) ^ (off >> 10)) & 1) != 0)
		off ^= (1 << 6);
	return off;
}


static uint32
create_bo(uint32 size, area_id* area)
{
	intel_gem_create create;
	memset(&create, 0, sizeof(create));
	create.magic = INTEL_PRIVATE_DATA_MAGIC;
	create.size = sizeof(create);
	create.bo_size = size;
	if (ioctl(sFd, INTEL_GEM_CREATE, &create, sizeof(create)) != 0)
		return 0;
	*area = create.area;
	return create.handle;
}


static status_t
set_tiling(uint32 handle, uint32 mode, uint32 stride, uint32* swizzle)
{
	intel_gem_set_tiling tiling;
	memset(&tiling, 0, sizeof(tiling));
	tiling.magic = INTEL_PRIVATE_DATA_MAGIC;
	tiling.size = sizeof(tiling);
	tiling.handle = handle;
	tiling.tiling_mode = mode;
	tiling.stride = stride;
	if (ioctl(sFd, INTEL_GEM_SET_TILING, &tiling, sizeof(tiling)) != 0)
		return errno;
	if (swizzle != NULL)
		*swizzle = tiling.swizzle_mode;
	return B_OK;
}


static void*
mmap_wc(uint32 handle, uint32 size)
{
	drm_i915_gem_mmap mmap;
	memset(&mmap, 0, sizeof(mmap));
	mmap.handle = handle;
	mmap.offset = 0;
	mmap.size = size;
	mmap.flags = I915_MMAP_WC;
	if (ioctl(sFd, DRM_IOCTL_I915_GEM_MMAP, &mmap, sizeof(mmap)) != 0)
		return NULL;
	return (void*)(addr_t)mmap.addr_ptr;
}


static status_t
read_bo(uint32 handle, void* dest, uint32 size)
{
	intel_gem_read_bo read;
	memset(&read, 0, sizeof(read));
	read.magic = INTEL_PRIVATE_DATA_MAGIC;
	read.size = sizeof(read);
	read.handle = handle;
	read.offset = 0;
	read.read_size = size;
	read.dest_ptr = (uint64)(addr_t)dest;
	if (ioctl(sFd, INTEL_GEM_READ_BO, &read, sizeof(read)) != 0)
		return errno;
	return B_OK;
}


int
main()
{
	sFd = open(kDevice, O_RDWR);
	if (sFd < 0) {
		printf("cannot open %s\n", kDevice);
		return 1;
	}

	// Confirm the driver now advertises fence registers.
	intel_gem_get_param param;
	memset(&param, 0, sizeof(param));
	param.magic = INTEL_PRIVATE_DATA_MAGIC;
	param.size = sizeof(param);
	param.param = INTEL_GEM_PARAM_NUM_FENCES;
	ioctl(sFd, INTEL_GET_PARAM, &param, sizeof(param));
	printf("NUM_FENCES = %" B_PRIu64 "\n", param.value);
	CHECK(param.value == INTEL_FENCE_COUNT, "driver reports 16 fence registers");

	const uint32 stride = 1024;			// 2 X-tiles wide
	const uint32 height = 32;			// 4 tile-rows
	const uint32 size = stride * height;	// 32 KiB

	uint8* linear = new uint8[size];
	for (uint32 i = 0; i < size; i++)
		linear[i] = (uint8)((i * 61 + (i >> 8) * 7) & 0xff);

	uint8* readback = new uint8[size];

	// ---- tiled case ----
	area_id area = -1;
	uint32 handle = create_bo(size, &area);
	CHECK(handle != 0, "create tiled BO");

	uint32 swizzle = 0;
	status_t status = set_tiling(handle, INTEL_GEM_TILING_X, stride, &swizzle);
	CHECK(status == B_OK, "SET_TILING(X) programs a fence");
	CHECK(swizzle == INTEL_GEM_BIT_6_SWIZZLE_9_10,
		"reported swizzle is 9_10 (Ironlake X-tiling)");

	uint8* aperture = (uint8*)mmap_wc(handle, size);
	CHECK(aperture != NULL, "map fenced aperture window (MMAP_WC)");

	if (aperture != NULL) {
		// write the linear image through the fenced (de-tiled) window
		memcpy(aperture, linear, size);

		// round-trip: read it back through the same window
		memcpy(readback, aperture, size);
		CHECK(memcmp(readback, linear, size) == 0,
			"round-trip through fence is identity (tile then de-tile)");

		// raw memory: must be the tiled rearrangement, i.e. NOT linear
		memset(readback, 0, size);
		status = read_bo(handle, readback, size);
		CHECK(status == B_OK, "READ_BO raw bytes");
		uint32 diff = 0;
		for (uint32 i = 0; i < size; i++)
			if (readback[i] != linear[i])
				diff++;
		printf("  raw vs linear: %" B_PRIu32 "/%" B_PRIu32 " bytes differ\n",
			diff, size);
		CHECK(diff > size / 4,
			"raw memory is tiled (differs substantially from linear)");

		// spot-check exact X-tile+swizzle placement at several positions
		int spotOk = 0, spotTotal = 0;
		const uint32 cols[] = { 0, 4, 508, 512, 1020 };
		const uint32 rows[] = { 0, 1, 7, 8, 17, 31 };
		for (uint32 ci = 0; ci < 5; ci++) {
			for (uint32 ri = 0; ri < 6; ri++) {
				uint32 lin = rows[ri] * stride + cols[ci];
				uint32 tof = x_tile_offset(cols[ci], rows[ri], stride);
				spotTotal++;
				if (readback[tof] == linear[lin])
					spotOk++;
			}
		}
		printf("  spot checks: %d/%d positions match X-tile+swizzle layout\n",
			spotOk, spotTotal);
		CHECK(spotOk == spotTotal,
			"every spot-checked byte lands at its computed tiled offset");
	}

	// ---- control: untiled BO, no fence ----
	area_id area2 = -1;
	uint32 handle2 = create_bo(size, &area2);
	uint8* aperture2 = (uint8*)mmap_wc(handle2, size);
	CHECK(handle2 != 0 && aperture2 != NULL, "create + map untiled control BO");
	if (aperture2 != NULL) {
		memcpy(aperture2, linear, size);
		memset(readback, 0, size);
		read_bo(handle2, readback, size);
		CHECK(memcmp(readback, linear, size) == 0,
			"untiled control: raw memory equals linear (no fence, no tiling)");
	}

	printf("\n%d passed, %d failed\n", sPass, sFail);
	delete[] linear;
	delete[] readback;
	close(sFd);
	return sFail == 0 ? 0 : 1;
}
