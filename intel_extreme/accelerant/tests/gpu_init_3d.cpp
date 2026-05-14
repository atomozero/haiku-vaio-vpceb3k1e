/*
 * gpu_init_3d — Apply Gen5 3D workarounds via MI_LOAD_REGISTER_IMM.
 *
 * Run ONCE after boot, before any 3D/OpenGL program.
 * Writes ILK workaround registers via LRI in the ring buffer,
 * bypassing the broken userspace MMIO.
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
	printf("=== Gen5 3D Pipeline Init ===\n\n");

	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) { printf("Cannot open device\n"); return 1; }

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(fd); return 1;
	}

	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("init3d shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return 1;
	}
	gInfo->regs_area = clone_area("init3d regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(fd); return 1;
	}

	ring_buffer &ring = gInfo->shared_info->primary_ring_buffer;

	// Read current register values
	printf("Before:\n");
	printf("  MI_MODE  (0x209C) = 0x%08x\n", read32(0x209C));
	printf("  3D_CHKN2 (0x208C) = 0x%08x\n", read32(0x208C));
	printf("  CACHE_M0 (0x2120) = 0x%08x\n", read32(0x2120));
	printf("  EIR      (0x20B0) = 0x%08x\n", read32(0x20B0));

	// Sync ring position
	uint32 hwTail = read32(ring.register_base + RING_BUFFER_TAIL)
		& (ring.size - 1);
	uint32 hwHead = read32(ring.register_base + RING_BUFFER_HEAD)
		& INTEL_RING_BUFFER_HEAD_MASK;
	printf("\nRing: HEAD=0x%x TAIL=0x%x\n", hwHead, hwTail);

	// Write LRI commands to ring
	uint32* rb = (uint32*)ring.base;
	uint32 pos = hwTail / 4;
	uint32 mask = (ring.size / 4) - 1;

	#define EMIT(v) do { rb[pos & mask] = (v); pos++; } while(0)

	// MI_LOAD_REGISTER_IMM: opcode 0x22, 5 register pairs = length 10
	EMIT((0x22 << 23) | 10);

	// 1. MI_MODE (0x209C): WaTimedSingleVertexDispatch:ilk
	EMIT(0x209C);
	EMIT((1 << 22) | (1 << 6));

	// 2. _3D_CHICKEN2 (0x208C): WM_READ_PIPELINED
	EMIT(0x208C);
	EMIT((1 << 30) | (1 << 14));

	// 3. CACHE_MODE_0 (0x2120): WaDisable_RenderCache_OperationalFlush
	//    + WaDisableRenderCachePipelinedFlush — CRITICAL for PIPE_CONTROL
	EMIT(0x2120);
	EMIT((1 << 24) | (1 << 16) | (1 << 8));

	// 4. EIR (0x20B0): clear error flags
	EMIT(0x20B0);
	EMIT(0);

	// 5. EMR (0x20B4): clear error mask
	EMIT(0x20B4);
	EMIT(0);

	// Pad to QWord
	EMIT(0);  // MI_NOOP

	#undef EMIT

	uint32 newTail = (pos * 4) & (ring.size - 1);
	asm volatile("mfence" ::: "memory");

	// Kick GPU via ioctl
	intel_ring_tail tailData;
	tailData.magic = INTEL_PRIVATE_DATA_MAGIC;
	tailData.tail_value = newTail;
	if (ioctl(fd, INTEL_RING_WRITE_TAIL, &tailData, sizeof(tailData)) != 0) {
		printf("TAIL ioctl failed!\n");
		goto cleanup;
	}

	// Wait for GPU to execute
	snooze(10000);
	{
	uint32 headAfter = read32(ring.register_base + RING_BUFFER_HEAD)
		& INTEL_RING_BUFFER_HEAD_MASK;
	printf("\nAfter LRI: HEAD=0x%x (expected >= 0x%x)\n",
		headAfter, newTail);

	// Verify registers changed
	printf("\nAfter:\n");
	printf("  MI_MODE  (0x209C) = 0x%08x\n", read32(0x209C));
	printf("  3D_CHKN2 (0x208C) = 0x%08x\n", read32(0x208C));
	printf("  CACHE_M0 (0x2120) = 0x%08x\n", read32(0x2120));
	printf("  EIR      (0x20B0) = 0x%08x\n", read32(0x20B0));

	printf("\n%s\n",
		headAfter >= newTail ? "3D workarounds applied!" : "FAILED — HEAD didn't advance");
	}

cleanup:
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo);
	close(fd);
	return 0;
}
