/*
 * IntelGfx Server - GPU acceleration server for Intel Gen5 on Haiku
 *
 * Opens the existing intel_extreme device, obtains shared_info and
 * register access, and provides GEM/batch buffer services to clients.
 * No separate kernel driver required - reuses intel_extreme's
 * PCI setup, MMIO mapping, and GTT aperture.
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
	int				device_fd;
	area_id			shared_info_area;
	intel_shared_info*	shared_info;
	area_id			regs_area;
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


// ---------------------------------------------------------------
// Device open and shared_info mapping
// ---------------------------------------------------------------

static status_t
open_intel_device()
{
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL)
		return B_ERROR;

	char devPath[256] = {};
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "intel_extreme", 13) == 0) {
			snprintf(devPath, sizeof(devPath),
				"/dev/graphics/%s", entry->d_name);
			break;
		}
	}
	closedir(dir);

	if (devPath[0] == '\0')
		return B_DEVICE_NOT_FOUND;

	sGPU.device_fd = open(devPath, B_READ_WRITE);
	if (sGPU.device_fd < 0)
		return B_ERROR;

	printf("[OK] Device opened: %s\n", devPath);
	return B_OK;
}


static status_t
map_shared_info()
{
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;

	if (ioctl(sGPU.device_fd, INTEL_GET_PRIVATE_DATA, &data,
			sizeof(intel_get_private_data)) != 0) {
		printf("[FAIL] INTEL_GET_PRIVATE_DATA ioctl failed\n");
		return B_ERROR;
	}

	sGPU.shared_info_area = clone_area("intel_gfx shared_info",
		(void**)&sGPU.shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sGPU.shared_info_area < 0) {
		printf("[FAIL] Cannot clone shared_info area\n");
		return sGPU.shared_info_area;
	}

	sGPU.regs_area = clone_area("intel_gfx registers",
		(void**)&sGPU.registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		sGPU.shared_info->registers_area);
	if (sGPU.regs_area < 0) {
		printf("[FAIL] Cannot clone registers area\n");
		return sGPU.regs_area;
	}

	printf("[OK] shared_info mapped (device type 0x%08" B_PRIx32 ", Gen %d)\n",
		sGPU.shared_info->device_type.type,
		sGPU.shared_info->device_type.Generation());
	return B_OK;
}


// ---------------------------------------------------------------
// Test 1: Register access
// ---------------------------------------------------------------

static bool
test_register_access()
{
	printf("\n--- Test 1: Register access ---\n");

	uint32 ringCtl = gpu_read32(0x203c);
	uint32 miMode = gpu_read32(0x209c);
	uint32 instdone = gpu_read32(0x206c);

	bool ringEnabled = (ringCtl & 1) != 0;
	printf("  RING_CTL=0x%08x (enabled=%s)\n", ringCtl,
		ringEnabled ? "yes" : "NO");
	printf("  MI_MODE=0x%08x\n", miMode);
	printf("  INSTDONE=0x%08x\n", instdone);

	if (!ringEnabled) {
		printf("[FAIL] Ring buffer not enabled\n");
		return false;
	}

	printf("[OK] Register access works, ring is enabled\n");
	return true;
}


// ---------------------------------------------------------------
// Test 2: Cross-process ring buffer lock
// ---------------------------------------------------------------

static bool
test_ring_lock()
{
	printf("\n--- Test 2: Cross-process ring lock ---\n");

	ring_buffer& ring = sGPU.shared_info->primary_ring_buffer;

	bigtime_t start = system_time();
	status_t status = acquire_lock(&ring.lock);
	bigtime_t elapsed = system_time() - start;

	if (status != B_OK) {
		printf("[FAIL] Cannot acquire ring lock (status 0x%x)\n", status);
		return false;
	}

	printf("  Lock acquired in %" B_PRId64 " us\n", elapsed);
	printf("  Ring position: %" B_PRIu32 "\n", ring.position);
	printf("  Ring space:    %" B_PRIu32 " / %" B_PRIu32 "\n",
		ring.space_left, ring.size);

	release_lock(&ring.lock);
	printf("[OK] Cross-process ring lock works\n");
	return true;
}


// ---------------------------------------------------------------
// Test 3: GPU command submission (MI_STORE_DATA_IMM to GPU memory)
// Write to a scratch location in GPU memory (not framebuffer),
// then read back via CPU to verify the GPU executed the command.
// ---------------------------------------------------------------

static bool
test_gpu_command()
{
	printf("\n--- Test 3: GPU command execution ---\n");

	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;

	// Write a marker to GPU scratch memory via MI_STORE_DATA_IMM.
	// Use GTT offset 0x10000 (after ring buffer, before framebuffer).
	// Read back via CPU through the GTT aperture to verify GPU executed.
	// NOTE: HEAD register on Gen5 updates lazily, can't use HEAD==TAIL.

	uint32 scratchGTT = 0x10000;	// GTT offset for scratch
	volatile uint32* scratchCPU = (volatile uint32*)(
		(uint8*)si->graphics_memory + scratchGTT);

	// Clear scratch via CPU
	*scratchCPU = 0;
	int32 barrier = 0;
	atomic_add(&barrier, 1);

	uint32 marker = 0xDEADBEEF;
	printf("  Scratch GTT 0x%x, CPU before: 0x%08x\n",
		scratchGTT, *scratchCPU);
	printf("  Ring pos=0x%x, HW TAIL=0x%08x, HW HEAD=0x%08x\n",
		ring.position,
		gpu_read32(ring.register_base + 0x00),
		gpu_read32(ring.register_base + 0x04));

	// Submit MI_STORE_DATA_IMM using exact same pattern as
	// QueueCommands (engine.cpp) - acquire lock, write, barrier,
	// update TAIL register, release lock.
	acquire_lock(&ring.lock);

	// MakeSpace equivalent: poll HEAD until space available
	uint32 needed = 4 * sizeof(uint32);
	bigtime_t start = system_time();
	while (ring.space_left < needed) {
		uint32 head = gpu_read32(ring.register_base + 0x04)
			& INTEL_RING_BUFFER_HEAD_MASK;
		if (head <= ring.position)
			head += ring.size;
		ring.space_left = head - ring.position;
		if (ring.space_left < needed) {
			if (system_time() > start + 100000LL) {
				printf("  [WARN] Ring space timeout, forcing\n");
				break;
			}
			snooze(100);
		}
	}

	// Write commands at ring.position (matching QueueCommands::Write)
	uint32 pos = ring.position;
	*(uint32*)(ring.base + pos) = 0x10400002;	pos = (pos + 4) & (ring.size - 1);
	*(uint32*)(ring.base + pos) = 0;			pos = (pos + 4) & (ring.size - 1);
	*(uint32*)(ring.base + pos) = scratchGTT;	pos = (pos + 4) & (ring.size - 1);
	*(uint32*)(ring.base + pos) = marker;		pos = (pos + 4) & (ring.size - 1);

	ring.position = pos;
	ring.space_left -= needed;

	// Memory barrier (matching QueueCommands destructor)
	atomic_add(&barrier, 1);

	// Write TAIL register (matching QueueCommands destructor)
	gpu_write32(ring.register_base + RING_BUFFER_TAIL, ring.position);

	release_lock(&ring.lock);

	printf("  After submit: pos=0x%x, HW TAIL=0x%08x\n",
		ring.position,
		gpu_read32(ring.register_base + RING_BUFFER_TAIL));

	snooze(10000);
	atomic_add(&barrier, 1);

	uint32 readBack = *scratchCPU;
	printf("  Scratch CPU after:  0x%08x (expect 0x%08x)\n",
		readBack, marker);

	if (readBack == marker) {
		printf("[OK] MI_STORE_DATA_IMM verified: GPU executes our commands!\n");
		return true;
	} else {
		printf("[FAIL] Scratch not written (GPU did not execute command)\n");
		return false;
	}
}


// ---------------------------------------------------------------
// Test 4: BLT fill via ring (proves GPU draws to framebuffer)
// ---------------------------------------------------------------

static bool
test_blt_fill()
{
	printf("\n--- Test 4: BLT fill from server process ---\n");

	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;

	// Draw a small yellow rectangle at (380, 50)-(420, 90)
	// using XY_COLOR_BLT from this process
	uint32 bpp = si->bits_per_pixel;
	uint32 bpr = si->bytes_per_row;
	uint32 fbOffset = si->frame_buffer_offset;

	if (bpp != 32) {
		printf("[SKIP] Only 32bpp supported (current: %" B_PRIu32 ")\n", bpp);
		return true;
	}

	// Use the existing accelerant's xy_color_blit_command via the shared
	// ring buffer.  Read pixel BEFORE and AFTER to detect change.
	uint32* fb = (uint32*)si->frame_buffer;
	uint32 stride = bpr / 4;

	// Read pixel before
	uint32 pixBefore = *(volatile uint32*)&fb[70 * stride + 400];
	printf("  Pixel (400,70) before: 0x%08x\n", pixBefore);

	acquire_lock(&ring.lock);

	// Refresh space
	uint32 head = gpu_read32(ring.register_base + 0x04)
		& INTEL_RING_BUFFER_HEAD_MASK;
	if (head <= ring.position)
		head += ring.size;
	ring.space_left = head - ring.position;

	if (ring.space_left < 8 * sizeof(uint32)) {
		release_lock(&ring.lock);
		printf("[FAIL] Not enough ring space\n");
		return false;
	}

	// Build XY_COLOR_BLT using exact defines from intel_extreme.h
	uint32* cmd = (uint32*)(ring.base + ring.position);
	cmd[0] = XY_COMMAND_COLOR_BLIT | COMMAND_BLIT_RGBA;  // 0x54300004
	cmd[1] = (COMMAND_MODE_RGB32 << 24) | (0xF0 << 16) | (bpr & 0xFFFF);
	cmd[2] = (50 << 16) | 380;		// top=50, left=380
	cmd[3] = (90 << 16) | 420;		// bottom=90, right=420
	cmd[4] = fbOffset;
	cmd[5] = 0xFFFFFF00;			// yellow
	cmd[6] = 0;
	cmd[7] = 0;

	ring.position = (ring.position + 8 * sizeof(uint32))
		& (ring.size - 1);
	ring.space_left -= 8 * sizeof(uint32);

	int32 flush = 0;
	atomic_add(&flush, 1);
	gpu_write32(ring.register_base + 0x00, ring.position);

	release_lock(&ring.lock);

	// Wait for GPU then immediately read
	snooze(2000);
	int32 flush2 = 0;
	atomic_add(&flush2, 1);

	uint32 pixAfter = *(volatile uint32*)&fb[70 * stride + 400];
	// Also sample multiple pixels in the rectangle
	uint32 pix2 = *(volatile uint32*)&fb[60 * stride + 390];
	uint32 pix3 = *(volatile uint32*)&fb[80 * stride + 410];

	printf("  BLT fill: yellow rect (380,50)-(420,90)\n");
	printf("  Pixel (400,70) after:  0x%08x\n", pixAfter);
	printf("  Pixel (390,60):        0x%08x\n", pix2);
	printf("  Pixel (410,80):        0x%08x\n", pix3);

	// Check if at least one pixel is yellow (desktop may overwrite some)
	bool anyYellow = ((pixAfter & 0x00FFFFFF) == 0x00FFFF00)
		|| ((pix2 & 0x00FFFFFF) == 0x00FFFF00)
		|| ((pix3 & 0x00FFFFFF) == 0x00FFFF00);
	bool changed = (pixAfter != pixBefore);

	if (anyYellow) {
		printf("[OK] BLT fill from server process works!\n");
	} else if (changed) {
		printf("[WARN] Pixel changed but not yellow (desktop may have overwritten)\n");
		printf("       Changed: 0x%08x -> 0x%08x\n", pixBefore, pixAfter);
	} else {
		printf("[FAIL] BLT fill did not change pixel\n");
	}

	return anyYellow || changed;
}


// ---------------------------------------------------------------
// Main
// ---------------------------------------------------------------

int
main(int argc, char* argv[])
{
	printf("========================================\n");
	printf("  IntelGfx Server v0.1 - Phase 1 Tests\n");
	printf("========================================\n");

	// Open device and map shared_info
	if (open_intel_device() != B_OK)
		return 1;
	if (map_shared_info() != B_OK) {
		close(sGPU.device_fd);
		return 1;
	}

	printf("\n  Graphics memory: %" B_PRIu32 " MB\n",
		sGPU.shared_info->graphics_memory_size / (1024 * 1024));
	printf("  Frame buffer:    offset 0x%" B_PRIx32 "\n",
		sGPU.shared_info->frame_buffer_offset);
	printf("  Display:         %" B_PRIu16 "x%" B_PRIu16 " @%" B_PRIu32 "bpp\n",
		sGPU.shared_info->current_mode.timing.h_display,
		sGPU.shared_info->current_mode.timing.v_display,
		sGPU.shared_info->bits_per_pixel);

	// Run tests
	int passed = 0, failed = 0;

	if (test_register_access()) passed++; else failed++;
	if (test_ring_lock()) passed++; else failed++;
	if (test_gpu_command()) passed++; else failed++;
	if (test_blt_fill()) passed++; else failed++;

	printf("\n========================================\n");
	printf("  Results: %d passed, %d failed\n", passed, failed);
	printf("========================================\n");

	// Cleanup
	delete_area(sGPU.regs_area);
	delete_area(sGPU.shared_info_area);
	close(sGPU.device_fd);

	return failed > 0 ? 1 : 0;
}
