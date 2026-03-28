/*
 * IntelGfx Server - GPU acceleration server for Intel Gen5 on Haiku
 *
 * Opens the existing intel_extreme device, obtains shared_info and
 * register access, and provides GEM/batch buffer services to clients.
 * No separate kernel driver required - reuses intel_extreme's
 * PCI setup, MMIO mapping, and GTT aperture.
 *
 * Architecture follows X547/RadeonGfx: userspace BApplication server
 * with IPC to clients via accelerant2 interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <OS.h>

// Include intel_extreme shared definitions
#include "accelerant.h"
#include "intel_extreme.h"


struct gpu_info {
	int				device_fd;
	area_id			shared_info_area;
	intel_shared_info*	shared_info;
	area_id			regs_area;
	volatile uint8*		registers;
	addr_t			gtt_base;		// GTT aperture virtual address
	uint32			gtt_size;		// GTT aperture size
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
open_intel_device()
{
	// Find the intel_extreme device node
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL) {
		fprintf(stderr, "IntelGfx: cannot open /dev/graphics\n");
		return B_ERROR;
	}

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

	if (devPath[0] == '\0') {
		fprintf(stderr, "IntelGfx: no intel_extreme device found\n");
		return B_DEVICE_NOT_FOUND;
	}

	printf("IntelGfx: opening %s\n", devPath);

	sGPU.device_fd = open(devPath, B_READ_WRITE);
	if (sGPU.device_fd < 0) {
		fprintf(stderr, "IntelGfx: cannot open %s\n", devPath);
		return B_ERROR;
	}

	return B_OK;
}


static status_t
map_shared_info()
{
	// Get shared_info area from kernel driver (same as accelerant does)
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;

	if (ioctl(sGPU.device_fd, INTEL_GET_PRIVATE_DATA, &data,
			sizeof(intel_get_private_data)) != 0) {
		fprintf(stderr, "IntelGfx: INTEL_GET_PRIVATE_DATA failed\n");
		return B_ERROR;
	}

	// Clone shared_info area
	sGPU.shared_info_area = clone_area("intel_gfx shared_info",
		(void**)&sGPU.shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sGPU.shared_info_area < 0) {
		fprintf(stderr, "IntelGfx: cannot clone shared_info area\n");
		return sGPU.shared_info_area;
	}

	// Clone registers area
	sGPU.regs_area = clone_area("intel_gfx registers",
		(void**)&sGPU.registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		sGPU.shared_info->registers_area);
	if (sGPU.regs_area < 0) {
		fprintf(stderr, "IntelGfx: cannot clone registers area\n");
		return sGPU.regs_area;
	}

	// Store GTT aperture info
	sGPU.gtt_base = (addr_t)sGPU.shared_info->graphics_memory;
	sGPU.gtt_size = sGPU.shared_info->graphics_memory_size;

	return B_OK;
}


static void
dump_gpu_info()
{
	intel_shared_info* si = sGPU.shared_info;

	printf("IntelGfx: GPU Information\n");
	printf("  Device type:     0x%08" B_PRIx32 "\n", si->device_type.type);
	printf("  Generation:      %d\n", si->device_type.Generation());
	printf("  Graphics memory: %p (%" B_PRIu32 " MB)\n",
		(void*)sGPU.gtt_base, sGPU.gtt_size / (1024 * 1024));
	printf("  Frame buffer:    offset 0x%" B_PRIx32 "\n",
		si->frame_buffer_offset);
	printf("  Ring buffer:     base 0x%04" B_PRIx32
		", offset 0x%" B_PRIx32 ", size 0x%" B_PRIx32 "\n",
		si->primary_ring_buffer.register_base,
		si->primary_ring_buffer.offset,
		si->primary_ring_buffer.size);

	// Read key registers
	printf("  RING_CTL:        0x%08" B_PRIx32 "\n", gpu_read32(0x203c));
	printf("  RING_HEAD:       0x%08" B_PRIx32 "\n", gpu_read32(0x2034));
	printf("  RING_TAIL:       0x%08" B_PRIx32 "\n", gpu_read32(0x2030));
	printf("  HWS_PGA:         0x%08" B_PRIx32 "\n", gpu_read32(0x2080));
	printf("  MI_MODE:         0x%08" B_PRIx32 "\n", gpu_read32(0x209c));
	printf("  INSTDONE:        0x%08" B_PRIx32 "\n", gpu_read32(0x206c));
}


static status_t
test_ring_access()
{
	intel_shared_info* si = sGPU.shared_info;
	ring_buffer& ring = si->primary_ring_buffer;

	printf("IntelGfx: Testing ring buffer access...\n");

	// Acquire the ring lock (shared with accelerant/app_server)
	if (acquire_lock(&ring.lock) != B_OK) {
		fprintf(stderr, "IntelGfx: cannot acquire ring lock\n");
		return B_ERROR;
	}

	printf("  Ring lock acquired (cross-process lock works!)\n");
	printf("  Ring position: %" B_PRIu32 "\n", ring.position);
	printf("  Ring space:    %" B_PRIu32 "\n", ring.space_left);

	// Write a MI_STORE_DATA_IMM to framebuffer to prove we can submit
	// GPU commands from this separate process
	uint32 fbOffset = si->frame_buffer_offset;
	uint32 bpr = si->bytes_per_row;
	// Target pixel (400, 50) - white dot
	uint32 pixelAddr = fbOffset + 50 * bpr + 400 * 4;

	uint32* ringBase = (uint32*)(ring.base + ring.position);

	// MI_STORE_DATA_IMM: write white pixel
	ringBase[0] = 0x10400002;	// MI_STORE_DATA_IMM | Use_GGTT | len=2
	ringBase[1] = 0;			// reserved
	ringBase[2] = pixelAddr;	// GGTT address
	ringBase[3] = 0xFFFFFFFF;	// white pixel

	ring.position = (ring.position + 4 * sizeof(uint32))
		& (ring.size - 1);
	ring.space_left -= 4 * sizeof(uint32);

	// Memory barrier
	int32 flush = 0;
	atomic_add(&flush, 1);

	// Write TAIL to submit
	gpu_write32(ring.register_base + 0x00, ring.position);  // RING_BUFFER_TAIL

	release_lock(&ring.lock);

	printf("  MI_STORE_DATA_IMM submitted: pixel at (400,50)\n");
	snooze(10000);

	// Read back the pixel via CPU
	uint32* fb = (uint32*)si->frame_buffer;
	uint32 stride = bpr / 4;
	uint32 pixel = *(volatile uint32*)&fb[50 * stride + 400];
	printf("  Pixel readback: 0x%08" B_PRIx32 " (%s)\n",
		pixel, pixel == 0xFFFFFFFF ? "WHITE - SUCCESS!" : "FAIL");

	return pixel == 0xFFFFFFFF ? B_OK : B_ERROR;
}


int
main(int argc, char* argv[])
{
	printf("=== IntelGfx Server v0.1 ===\n");

	status_t status;

	// Phase 1.1: Open device and map shared_info
	status = open_intel_device();
	if (status != B_OK)
		return 1;

	status = map_shared_info();
	if (status != B_OK) {
		close(sGPU.device_fd);
		return 1;
	}

	// Phase 1.2: Dump GPU info
	dump_gpu_info();

	// Phase 1.3: Test ring buffer access from this process
	status = test_ring_access();
	if (status == B_OK) {
		printf("\nIntelGfx: Cross-process GPU command submission WORKS!\n");
		printf("IntelGfx: Ready for Phase 2 (GEM manager, batch execution)\n");
	} else {
		printf("\nIntelGfx: Ring buffer test FAILED\n");
	}

	// Cleanup
	delete_area(sGPU.regs_area);
	delete_area(sGPU.shared_info_area);
	close(sGPU.device_fd);

	return status == B_OK ? 0 : 1;
}
