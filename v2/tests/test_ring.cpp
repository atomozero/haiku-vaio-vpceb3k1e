/*
 * test_ring.cpp — Ring buffer submission and wrap-around test.
 *
 * Submits many MI_NOOP batches to stress the ring, verifying that
 * wrap-around works correctly without GPU hang.
 *
 * Build:
 *   cd intel_extreme/accelerant && make   # build .o files
 *   g++ -Wall -O2 -I../../intel_extreme/accelerant \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics/common \
 *       -I/boot/system/develop/headers/os/add-ons/graphics \
 *       -I/boot/system/develop/headers/os/drivers \
 *       -o test_ring test_ring.cpp \
 *       ../../intel_extreme/accelerant/gpu_ring.cpp \
 *       -lbe -lstdc++
 *
 * Run:
 *   ./test_ring
 */

#include "gpu_ring.h"
#include "intel_extreme.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <OS.h>


#define TEST_DEVICE_PATH	"/dev/graphics/intel_extreme_000200"
#define MI_NOOP				0x00000000
#define MI_STORE_DATA_IMM_GGTT	0x10400002
#define MARKER_SENTINEL		0xDEADBEEFu
#define MARKER_TAG(n)		(0xBEEF0000u | (n))


static int
open_device(void)
{
	int fd = open(TEST_DEVICE_PATH, O_RDWR);
	if (fd < 0)
		fd = open("/dev/graphics/intel_extreme/0", O_RDWR);
	return fd;
}


static addr_t
alloc_gtt(int fd, size_t size, size_t alignment)
{
	intel_allocate_graphics_memory alloc;
	alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
	alloc.size = size;
	alloc.alignment = alignment;
	alloc.flags = 0;
	if (ioctl(fd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &alloc, sizeof(alloc)) < 0)
		return 0;
	return alloc.buffer_base;
}


static uint8*
get_graphics_memory(int fd)
{
	intel_get_private_data priv;
	priv.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &priv, sizeof(priv)) != 0)
		return NULL;

	intel_shared_info* shared;
	area_id area = clone_area("test_ring shared",
		(void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA, priv.shared_info_area);
	if (area < 0)
		return NULL;

	return (uint8*)shared->graphics_memory;
}


/* Test 1: Basic ring submission (MI_NOOP) */
static bool
test_basic_submit(gpu_ring* ring)
{
	printf("  [1] Basic MI_NOOP submit: ");

	uint32 head_before = gpu_ring_read_head(ring);

	status_t st = gpu_ring_begin(ring, 4);
	if (st != B_OK) {
		printf("FAIL (begin: %s)\n", strerror(st));
		return false;
	}

	gpu_ring_emit(ring, MI_NOOP);
	gpu_ring_emit(ring, MI_NOOP);

	st = gpu_ring_advance(ring);
	if (st != B_OK) {
		printf("FAIL (advance: %s)\n", strerror(st));
		return false;
	}

	bool idle = gpu_ring_wait_idle(ring, 100000);
	uint32 head_after = gpu_ring_read_head(ring);

	if (!idle) {
		printf("FAIL (timeout, HEAD 0x%x → 0x%x)\n",
			head_before, head_after);
		return false;
	}

	printf("PASS (HEAD 0x%x → 0x%x)\n", head_before, head_after);
	return true;
}


/* Test 2: Marker write via MI_STORE_DATA_IMM */
static bool
test_marker(gpu_ring* ring, addr_t marker_base, uint8* gfx_mem)
{
	printf("  [2] MI_STORE_DATA_IMM marker: ");

	uint32 marker_gtt = (uint32)(marker_base - (addr_t)gfx_mem);
	volatile uint32* marker_ptr = (volatile uint32*)(gfx_mem + marker_gtt);

	*marker_ptr = MARKER_SENTINEL;
	asm volatile("mfence" ::: "memory");

	status_t st = gpu_ring_begin(ring, 6);
	if (st != B_OK) {
		printf("FAIL (begin: %s)\n", strerror(st));
		return false;
	}

	gpu_ring_emit(ring, MI_STORE_DATA_IMM_GGTT);
	gpu_ring_emit(ring, 0);
	gpu_ring_emit(ring, marker_gtt);
	gpu_ring_emit(ring, MARKER_TAG(1));

	st = gpu_ring_advance(ring);
	if (st != B_OK) {
		printf("FAIL (advance: %s)\n", strerror(st));
		return false;
	}

	/* Wait for marker */
	bigtime_t deadline = system_time() + 100000;
	while (system_time() < deadline) {
		if (*marker_ptr == MARKER_TAG(1))
			break;
		snooze(50);
	}

	uint32 val = *marker_ptr;
	if (val != MARKER_TAG(1)) {
		printf("FAIL (marker=0x%x, want 0x%x)\n", val, MARKER_TAG(1));
		return false;
	}

	printf("PASS (marker=0x%x)\n", val);
	return true;
}


/* Test 3: Ring wrap-around stress */
static bool
test_wrap_around(gpu_ring* ring, addr_t marker_base, uint8* gfx_mem)
{
	printf("  [3] Ring wrap-around (%u byte ring): ", ring->size);

	uint32 marker_gtt = (uint32)(marker_base - (addr_t)gfx_mem);
	volatile uint32* marker_ptr = (volatile uint32*)(gfx_mem + marker_gtt);

	/*
	 * Submit batches that each consume ~256 bytes (64 DWORDs) of ring space.
	 * For a 64KB ring, wrapping occurs after ~250 submissions.
	 * We do 1000 to trigger multiple wraps.
	 */
	uint32 submit_count = 1000;
	uint32 fail_count = 0;
	uint32 wrap_count = 0;
	uint32 prev_pos = ring->pos;

	for (uint32 i = 0; i < submit_count; i++) {
		*marker_ptr = MARKER_SENTINEL;
		asm volatile("mfence" ::: "memory");

		/* 64 DW batch: marker + 60 NOOPs */
		status_t st = gpu_ring_begin(ring, 66);
		if (st != B_OK) {
			printf("FAIL (begin #%u: %s, pos=0x%x)\n",
				i, strerror(st), ring->pos);
			return false;
		}

		/* Detect wrap */
		if (ring->pos < prev_pos)
			wrap_count++;
		prev_pos = ring->pos;

		/* Marker + padding */
		gpu_ring_emit(ring, MI_STORE_DATA_IMM_GGTT);
		gpu_ring_emit(ring, 0);
		gpu_ring_emit(ring, marker_gtt);
		gpu_ring_emit(ring, MARKER_TAG(i + 100));
		for (uint32 j = 0; j < 60; j++)
			gpu_ring_emit(ring, MI_NOOP);

		st = gpu_ring_advance(ring);
		if (st != B_OK) {
			printf("FAIL (advance #%u: %s)\n", i, strerror(st));
			return false;
		}

		/* Wait for marker */
		bigtime_t deadline = system_time() + 200000;
		bool ok = false;
		while (system_time() < deadline) {
			if (*marker_ptr == MARKER_TAG(i + 100)) {
				ok = true;
				break;
			}
			snooze(10);
		}

		if (!ok)
			fail_count++;

		/* Bail early if GPU is dead */
		if (fail_count > 3) {
			printf("FAIL (%u/%u, %u wraps, GPU hung at submit #%u)\n",
				submit_count - fail_count, submit_count,
				wrap_count, i);
			return false;
		}
	}

	if (fail_count > 0) {
		printf("FAIL (%u/%u passed, %u wraps)\n",
			submit_count - fail_count, submit_count, wrap_count);
		return false;
	}

	printf("PASS (%u submits, %u wraps)\n", submit_count, wrap_count);
	return true;
}


int
main(int argc, char** argv)
{
	printf("=== Ring Buffer Test ===\n\n");

	int fd = open_device();
	if (fd < 0) {
		printf("Cannot open GPU device\n");
		return 1;
	}

	uint8* gfx_mem = get_graphics_memory(fd);
	if (gfx_mem == NULL) {
		printf("Cannot get graphics memory\n");
		close(fd);
		return 1;
	}

	addr_t marker_base = alloc_gtt(fd, 64, 64);
	if (marker_base == 0) {
		printf("Cannot allocate marker BO\n");
		close(fd);
		return 1;
	}

	gpu_ring ring;
	status_t st = gpu_ring_init(&ring, fd);
	if (st != B_OK) {
		printf("gpu_ring_init failed: %s\n", strerror(st));
		close(fd);
		return 1;
	}

	printf("Ring: pos=0x%x size=%u (%uKB)\n\n",
		ring.pos, ring.size, ring.size / 1024);

	int pass = 0, fail = 0;

	if (test_basic_submit(&ring))
		pass++;
	else
		fail++;

	if (test_marker(&ring, marker_base, gfx_mem))
		pass++;
	else
		fail++;

	if (test_wrap_around(&ring, marker_base, gfx_mem))
		pass++;
	else
		fail++;

	printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);

	close(fd);
	return fail > 0 ? 1 : 0;
}
