/*
 * test_ring_ioctl — Test INTEL_RING_RESET and INTEL_RING_WRITE_TAIL ioctls.
 *
 * Run after reboot with patched kernel driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"

extern accelerant_info* gInfo;

int
main(int, char**)
{
	printf("=== Ring Ioctl Test ===\n\n");

	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) { printf("Cannot open device\n"); return 1; }

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		printf("GET_PRIVATE_DATA failed\n"); close(fd); return 1;
	}

	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("test shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		printf("clone shared_info failed\n"); free(gInfo); close(fd); return 1;
	}
	gInfo->regs_area = clone_area("test regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		printf("clone regs failed\n"); close(fd); return 1;
	}

	ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;
	uint32 ringReg = ring.register_base;

	// Test 1: read ring state
	uint32 head = read32(ringReg + RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
	uint32 tail = read32(ringReg + RING_BUFFER_TAIL) & (ring.size - 1);
	uint32 ctl = read32(ringReg + RING_BUFFER_CONTROL);
	printf("Before reset: HEAD=0x%x TAIL=0x%x CTL=0x%x pos=%u\n",
		head, tail, ctl, ring.position);

	// Test 2: direct write32 test (should work now with B_WRITE_AREA)
	uint32 scratch_before = read32(0x2094);
	write32(0x2094, 0xDEADBEEF);
	uint32 scratch_after = read32(0x2094);
	printf("\nDirect MMIO write test (0x2094):\n");
	printf("  before=0x%x, wrote 0xDEADBEEF, read=0x%x → %s\n",
		scratch_before, scratch_after,
		scratch_after == 0xDEADBEEF ? "WRITABLE!" : "still read-only");

	// Test 3: INTEL_RING_RESET ioctl
	printf("\nCalling INTEL_RING_RESET ioctl...\n");
	intel_get_private_data resetData;
	resetData.magic = INTEL_PRIVATE_DATA_MAGIC;
	status_t st = ioctl(fd, INTEL_RING_RESET, &resetData, sizeof(resetData));
	printf("  ioctl returned: %d\n", (int)st);

	head = read32(ringReg + RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
	tail = read32(ringReg + RING_BUFFER_TAIL) & (ring.size - 1);
	ctl = read32(ringReg + RING_BUFFER_CONTROL);
	printf("  After reset: HEAD=0x%x TAIL=0x%x CTL=0x%x pos=%u\n",
		head, tail, ctl, ring.position);

	// Test 4: write a MI_NOOP + MI_NOOP to ring, then kick with TAIL ioctl
	if (head == 0 && ring.position == 0) {
		printf("\nRing is clean, testing TAIL write...\n");
		uint32* ringMem = (uint32*)ring.base;
		ringMem[0] = 0;  // MI_NOOP
		ringMem[1] = 0;  // MI_NOOP
		asm volatile("mfence" ::: "memory");

		intel_ring_tail tailData;
		tailData.magic = INTEL_PRIVATE_DATA_MAGIC;
		tailData.tail_value = 8;  // 2 DWORDs = 8 bytes
		st = ioctl(fd, INTEL_RING_WRITE_TAIL, &tailData, sizeof(tailData));
		printf("  TAIL write ioctl returned: %d\n", (int)st);

		snooze(10000);
		head = read32(ringReg + RING_BUFFER_HEAD) & INTEL_RING_BUFFER_HEAD_MASK;
		tail = read32(ringReg + RING_BUFFER_TAIL) & (ring.size - 1);
		printf("  After TAIL kick: HEAD=0x%x TAIL=0x%x\n", head, tail);
		printf("  GPU %s the NOOPs!\n",
			head >= 8 ? "EXECUTED" : "did NOT execute");
	}

	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo);
	close(fd);
	printf("\nDone.\n");
	return 0;
}
