/*
 * bench_gpu - CPU vs GPU BLT benchmark for Intel Gen5 on Haiku
 *
 * Compares CPU memset vs GPU BLT fill vs GPU BLT blit.
 * Uses batched ring submission with HEAD==TAIL wait after each batch.
 * Safe: won't overflow the ring or hang the GPU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <OS.h>

#include "accelerant.h"
#include "intel_extreme.h"


struct gpu_info {
	int					device_fd;
	area_id				shared_info_area;
	intel_shared_info*	shared_info;
	area_id				regs_area;
	volatile uint8*		registers;
};

static gpu_info sGPU;

static inline uint32
gpu_read32(uint32 offset)
{
	return *(volatile uint32*)(sGPU.registers + offset);
}

static inline void
gpu_write32(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sGPU.registers + offset) = value;
}


static status_t
open_and_map()
{
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL) return B_ERROR;
	char devPath[256] = {};
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "intel_extreme", 13) == 0) {
			snprintf(devPath, sizeof(devPath), "/dev/graphics/%s",
				entry->d_name);
			break;
		}
	}
	closedir(dir);
	if (devPath[0] == '\0') return B_DEVICE_NOT_FOUND;

	sGPU.device_fd = open(devPath, B_READ_WRITE);
	if (sGPU.device_fd < 0) return B_ERROR;

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sGPU.device_fd, INTEL_GET_PRIVATE_DATA, &data,
			sizeof(data)) != 0)
		return B_ERROR;

	sGPU.shared_info_area = clone_area("bench shared_info",
		(void**)&sGPU.shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sGPU.shared_info_area < 0) return sGPU.shared_info_area;

	sGPU.regs_area = clone_area("bench registers",
		(void**)&sGPU.registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		sGPU.shared_info->registers_area);
	if (sGPU.regs_area < 0) return sGPU.regs_area;

	return B_OK;
}


// ---------------------------------------------------------------
// Safe ring buffer helpers
// ---------------------------------------------------------------

// Wait for HEAD to catch up to current TAIL. Returns false on timeout.
static bool
wait_gpu_idle(uint32 timeout_us = 100000)
{
	ring_buffer& ring = sGPU.shared_info->primary_ring_buffer;
	uint32 tail = ring.position;
	bigtime_t deadline = system_time() + timeout_us;

	while (system_time() < deadline) {
		uint32 head = gpu_read32(ring.register_base + 0x04)
			& INTEL_RING_BUFFER_HEAD_MASK;
		if (head == tail)
			return true;
		snooze(2);
	}
	return false;
}


// Ensure at least `bytes` free in ring, polling HEAD.
static bool
ring_make_space(ring_buffer& ring, uint32 bytes)
{
	bigtime_t deadline = system_time() + 100000;
	while (ring.space_left < bytes) {
		uint32 head = gpu_read32(ring.register_base + 0x04)
			& INTEL_RING_BUFFER_HEAD_MASK;
		if (head <= ring.position)
			head += ring.size;
		ring.space_left = head - ring.position;
		if (system_time() > deadline)
			return false;
		snooze(2);
	}
	return true;
}


static inline void
ring_emit(ring_buffer& ring, uint32 value)
{
	*(uint32*)(ring.base + ring.position) = value;
	ring.position = (ring.position + 4) & (ring.size - 1);
	ring.space_left -= 4;
}


static void
ring_flush(ring_buffer& ring)
{
	int32 barrier = 0;
	atomic_add(&barrier, 1);
	gpu_write32(ring.register_base + RING_BUFFER_TAIL, ring.position);
}


// ---------------------------------------------------------------
// Pre-flight: verify GPU is alive
// ---------------------------------------------------------------

// Full ring reset: disable, clear, re-enable.
// This recovers from a hung command streamer.
static void
reset_ring()
{
	ring_buffer& ring = sGPU.shared_info->primary_ring_buffer;
	uint32 rb = ring.register_base;

	acquire_lock(&ring.lock);

	// Disable ring
	gpu_write32(rb + 0x0c, 0);
	gpu_read32(rb + 0x0c);
	snooze(2000);

	// Reset HEAD and TAIL
	gpu_write32(rb + 0x04, 0);
	gpu_read32(rb + 0x04);
	snooze(1000);
	gpu_write32(rb + 0x00, 0);
	gpu_read32(rb + 0x00);
	snooze(1000);

	// Preserve START, clear ring memory
	memset((void*)ring.base, 0, ring.size);

	// Re-enable
	gpu_write32(rb + 0x0c,
		((ring.size - 4096) & 0x001ff000) | 1);
	gpu_read32(rb + 0x0c);
	snooze(5000);

	// Reset software state
	ring.position = 0;
	ring.space_left = ring.size;

	release_lock(&ring.lock);
}


static bool
preflight()
{
	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;
	uint32 rb = ring.register_base;

	uint32 scratchGTT = 0x10000;
	volatile uint32* scratch = (volatile uint32*)(
		(uint8*)si->graphics_memory + scratchGTT);

	// First: check if GPU is already responsive
	*scratch = 0;
	int32 b = 0; atomic_add(&b, 1);

	acquire_lock(&ring.lock);
	ring_make_space(ring, 16);
	ring_emit(ring, 0x10400002);
	ring_emit(ring, 0);
	ring_emit(ring, scratchGTT);
	ring_emit(ring, 0xBEEF0001);
	ring_flush(ring);
	release_lock(&ring.lock);

	snooze(10000);
	atomic_add(&b, 1);

	if (*scratch == 0xBEEF0001) {
		printf("  [OK] GPU responsive\n");
	} else {
		// GPU not responding — try ring reset
		printf("  GPU not responding, resetting ring...\n");
		reset_ring();
		snooze(5000);

		// Retry
		*scratch = 0;
		atomic_add(&b, 1);

		acquire_lock(&ring.lock);
		ring_make_space(ring, 16);
		ring_emit(ring, 0x10400002);
		ring_emit(ring, 0);
		ring_emit(ring, scratchGTT);
		ring_emit(ring, 0xBEEF0002);
		ring_flush(ring);
		release_lock(&ring.lock);

		snooze(20000);
		atomic_add(&b, 1);

		if (*scratch == 0xBEEF0002) {
			printf("  [OK] GPU recovered after ring reset!\n");
		} else {
			printf("  [FAIL] GPU still dead (scratch=0x%08x)\n",
				*scratch);
			printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
				gpu_read32(rb + 4), gpu_read32(rb + 0),
				gpu_read32(rb + 0xc));
			printf("  Reboot required.\n");
			return false;
		}
	}

	// Test BLT + HEAD==TAIL wait
	acquire_lock(&ring.lock);
	ring_make_space(ring, 24);
	ring_emit(ring, XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA);
	ring_emit(ring, (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
		| (si->bytes_per_row & 0xFFFF));
	ring_emit(ring, (2 << 16) | 2);
	ring_emit(ring, (4 << 16) | 4);
	ring_emit(ring, si->frame_buffer_offset);
	ring_emit(ring, 0xFF00FF00);
	ring_flush(ring);
	release_lock(&ring.lock);

	bool ok = wait_gpu_idle(50000);
	printf("  [%s] BLT + wait_idle\n", ok ? "OK" : "WARN");
	return true;
}


// ---------------------------------------------------------------
// Benchmark sizes
// ---------------------------------------------------------------

struct bench_size {
	uint32		w;
	uint32		h;
	const char*	label;
};

static const bench_size kSizes[] = {
	{   16,   16, "16x16    " },
	{   64,   64, "64x64    " },
	{  256,  256, "256x256  " },
	{  512,  512, "512x512  " },
	{ 1024,  512, "1024x512 " },
	{ 1366,  768, "Fullscr  " },
};
static const int kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);

static const bigtime_t kBenchDuration = 2000000;  // 2 sec per test


// ---------------------------------------------------------------
// 1. CPU memset fill
// ---------------------------------------------------------------

static void
bench_cpu_fill()
{
	intel_shared_info* si = sGPU.shared_info;
	uint32 bpr = si->bytes_per_row;
	uint8* fb = (uint8*)si->frame_buffer;
	uint32 maxW = si->current_mode.timing.h_display;
	uint32 maxH = si->current_mode.timing.v_display;

	printf("\n--- CPU memset fill ---\n");
	printf("  %-12s %8s %10s\n", "Size", "Iters", "MB/s");
	fflush(stdout);

	for (int s = 0; s < kNumSizes; s++) {
		uint32 w = kSizes[s].w < maxW ? kSizes[s].w : maxW;
		uint32 h = kSizes[s].h < maxH ? kSizes[s].h : maxH;
		uint32 rowBytes = w * 4;
		int iters = 0;

		bigtime_t start = system_time();
		while (system_time() - start < kBenchDuration) {
			for (uint32 y = 0; y < h; y++)
				memset(fb + y * bpr, 0x80, rowBytes);
			iters++;
		}
		bigtime_t elapsed = system_time() - start;

		double totalMB = (double)rowBytes * h * iters / 1048576.0;
		double secs = (double)elapsed / 1000000.0;

		printf("  %-12s %8d %10.1f\n",
			kSizes[s].label, iters, totalMB / secs);
		fflush(stdout);
	}
}


// ---------------------------------------------------------------
// 2. GPU BLT fill (XY_COLOR_BLT)
//    Batch up to N fills, flush once, wait for completion, repeat.
// ---------------------------------------------------------------

static void
bench_blt_fill()
{
	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;
	uint32 bpr = si->bytes_per_row;
	uint32 fbOffset = si->frame_buffer_offset;
	uint32 maxW = si->current_mode.timing.h_display;
	uint32 maxH = si->current_mode.timing.v_display;

	// Max BLTs per batch: use at most half the ring.
	// Each BLT = 6 DWORDs = 24 bytes. Half ring = 32KB.
	// 32768 / 24 = ~1365. Use 1000 to be safe.
	const int kBatchSmall = 500;
	const int kBatchLarge = 20;   // for large rects

	printf("\n--- GPU BLT fill (XY_COLOR_BLT) ---\n");
	printf("  %-12s %8s %10s\n", "Size", "Iters", "MB/s");
	fflush(stdout);

	for (int s = 0; s < kNumSizes; s++) {
		uint32 w = kSizes[s].w < maxW ? kSizes[s].w : maxW;
		uint32 h = kSizes[s].h < maxH ? kSizes[s].h : maxH;
		uint32 pixels = w * h;
		int batchSize = (pixels > 100000) ? kBatchLarge : kBatchSmall;
		int totalIters = 0;
		bool gpuOk = true;

		bigtime_t start = system_time();
		while (system_time() - start < kBenchDuration && gpuOk) {
			acquire_lock(&ring.lock);

			for (int i = 0; i < batchSize; i++) {
				if (!ring_make_space(ring, 24)) {
					printf("  [ERR] Ring space timeout at iter %d\n",
						totalIters + i);
					gpuOk = false;
					break;
				}
				ring_emit(ring, XY_COMMAND_COLOR_BLIT
					| COMMAND_BLIT_RGBA);
				ring_emit(ring, (COMMAND_MODE_RGB32 << 24)
					| (0xF0 << 16) | (bpr & 0xFFFF));
				ring_emit(ring, 0);                 // top-left (0,0)
				ring_emit(ring, (h << 16) | w);     // bottom-right
				ring_emit(ring, fbOffset);
				ring_emit(ring, 0xFF000080 + (totalIters & 0x7F));
			}

			ring_flush(ring);
			release_lock(&ring.lock);

			if (gpuOk && !wait_gpu_idle(200000)) {
				printf("  [ERR] GPU wait timeout!\n");
				gpuOk = false;
				break;
			}
			totalIters += batchSize;
		}
		bigtime_t elapsed = system_time() - start;

		double totalMB = (double)w * h * 4.0 * totalIters / 1048576.0;
		double secs = (double)elapsed / 1000000.0;

		printf("  %-12s %8d %10.1f%s\n",
			kSizes[s].label, totalIters, totalMB / secs,
			gpuOk ? "" : " ERR");
		fflush(stdout);

		if (!gpuOk) break;
	}
}


// ---------------------------------------------------------------
// 3. GPU BLT blit (XY_SRC_COPY_BLT)
// ---------------------------------------------------------------

static void
bench_blt_copy()
{
	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;
	uint32 bpr = si->bytes_per_row;
	uint32 fbOffset = si->frame_buffer_offset;
	uint32 maxW = si->current_mode.timing.h_display;
	uint32 maxH = si->current_mode.timing.v_display;

	const int kBatchSmall = 400;  // 32 bytes each, 400*32=12.8KB
	const int kBatchLarge = 20;

	printf("\n--- GPU BLT blit (XY_SRC_COPY_BLT) ---\n");
	printf("  %-12s %8s %10s\n", "Size", "Iters", "MB/s");
	fflush(stdout);

	for (int s = 0; s < kNumSizes; s++) {
		uint32 w = kSizes[s].w < maxW ? kSizes[s].w : maxW;
		uint32 h = kSizes[s].h < maxH ? kSizes[s].h : maxH;
		uint32 pixels = w * h;

		// Non-overlapping src/dst
		uint32 dx = 0, dy = 0;
		if (w * 2 <= maxW)
			dx = w;
		else if (h * 2 <= maxH)
			dy = h;
		else {
			printf("  %-12s %8s %10s\n",
				kSizes[s].label, "--", "skip");
			fflush(stdout);
			continue;
		}

		int batchSize = (pixels > 100000) ? kBatchLarge : kBatchSmall;
		int totalIters = 0;
		bool gpuOk = true;

		bigtime_t start = system_time();
		while (system_time() - start < kBenchDuration && gpuOk) {
			acquire_lock(&ring.lock);

			for (int i = 0; i < batchSize; i++) {
				if (!ring_make_space(ring, 32)) {
					gpuOk = false;
					break;
				}
				ring_emit(ring, XY_COMMAND_SOURCE_BLIT
					| COMMAND_BLIT_RGBA);
				ring_emit(ring, (COMMAND_MODE_RGB32 << 24)
					| (0xCC << 16) | (bpr & 0xFFFF));
				ring_emit(ring, (dy << 16) | dx);
				ring_emit(ring, ((dy + h) << 16) | (dx + w));
				ring_emit(ring, fbOffset);
				ring_emit(ring, 0);               // src top-left (0,0)
				ring_emit(ring, (bpr & 0xFFFF));  // src pitch
				ring_emit(ring, fbOffset);        // src base
			}

			ring_flush(ring);
			release_lock(&ring.lock);

			if (gpuOk && !wait_gpu_idle(200000)) {
				printf("  [ERR] GPU wait timeout!\n");
				gpuOk = false;
				break;
			}
			totalIters += batchSize;
		}
		bigtime_t elapsed = system_time() - start;

		double totalMB = (double)w * h * 4.0 * totalIters / 1048576.0;
		double secs = (double)elapsed / 1000000.0;

		printf("  %-12s %8d %10.1f%s\n",
			kSizes[s].label, totalIters, totalMB / secs,
			gpuOk ? "" : " ERR");
		fflush(stdout);

		if (!gpuOk) break;
	}
}


// ---------------------------------------------------------------
// 4. Single BLT latency
// ---------------------------------------------------------------

static void
bench_latency()
{
	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;
	uint32 bpr = si->bytes_per_row;
	uint32 fbOffset = si->frame_buffer_offset;

	printf("\n--- Single BLT round-trip latency ---\n");
	fflush(stdout);

	int iters = 0;
	bigtime_t start = system_time();

	while (system_time() - start < kBenchDuration) {
		acquire_lock(&ring.lock);
		ring_make_space(ring, 24);
		ring_emit(ring, XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA);
		ring_emit(ring, (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16)
			| (bpr & 0xFFFF));
		ring_emit(ring, 0);
		ring_emit(ring, (4 << 16) | 4);  // tiny 4x4 fill
		ring_emit(ring, fbOffset);
		ring_emit(ring, 0xFF000000);
		ring_flush(ring);
		release_lock(&ring.lock);

		if (!wait_gpu_idle(10000))
			break;
		iters++;
	}
	bigtime_t elapsed = system_time() - start;

	double us_per = (double)elapsed / (double)iters;
	printf("  %d round-trips in %.1f ms\n", iters, elapsed / 1000.0);
	printf("  Latency: %.1f us/cmd (%.0f cmds/sec)\n",
		us_per, 1000000.0 / us_per);
	fflush(stdout);
}


// ---------------------------------------------------------------
// Main
// ---------------------------------------------------------------

int
main()
{
	printf("================================================\n");
	printf("  Intel Gen5 GPU Benchmark - CPU vs BLT Engine\n");
	printf("================================================\n");
	fflush(stdout);

	if (open_and_map() != B_OK) {
		printf("Cannot open intel_extreme device\n");
		return 1;
	}

	intel_shared_info* si = sGPU.shared_info;
	printf("  GPU:     Gen %d (device 0x%04x)\n",
		si->device_type.Generation(),
		si->device_type.type & 0xFFFF);
	printf("  Display: %dx%d @%dbpp, stride %d\n",
		si->current_mode.timing.h_display,
		si->current_mode.timing.v_display,
		si->bits_per_pixel, si->bytes_per_row);
	fflush(stdout);

	printf("\n--- Pre-flight check ---\n");
	fflush(stdout);
	if (!preflight()) return 1;

	bench_cpu_fill();
	bench_blt_fill();
	bench_blt_copy();
	bench_latency();

	printf("\n================================================\n");
	printf("  Benchmark complete\n");
	printf("================================================\n");

	delete_area(sGPU.regs_area);
	delete_area(sGPU.shared_info_area);
	close(sGPU.device_fd);
	return 0;
}
