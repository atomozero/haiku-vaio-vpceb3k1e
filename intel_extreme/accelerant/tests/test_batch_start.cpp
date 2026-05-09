/*
 * test_batch_start — Test MI_BATCH_BUFFER_START from userspace clone.
 *
 * This is the prerequisite for Mesa's GEM_EXECBUFFER2: crocus submits
 * all GPU commands via batch buffers, never directly in the ring.
 *
 * Test: allocate a GPU BO, write MI_STORE_DATA_IMM + MI_BATCH_BUFFER_END
 * into it, then submit MI_BATCH_BUFFER_START via the ring. If the marker
 * fires, batch buffer execution from userspace clone works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <OS.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "commands.h"
#include "media_pipeline.h"
#include "gpu_bo.h"

extern accelerant_info* gInfo;


static bool
init_gpu(void)
{
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) {
		printf("Cannot open GPU device: %s\n", strerror(errno));
		return false;
	}
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(fd); return false;
	}
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;
	gInfo->shared_info_area = clone_area("test shared",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		free(gInfo); close(fd); return false;
	}
	gInfo->regs_area = clone_area("test regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		delete_area(gInfo->shared_info_area);
		free(gInfo); close(fd); return false;
	}
	return true;
}

static void cleanup_gpu(void) {
	if (!gInfo) return;
	int fd = gInfo->device;
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo); gInfo = NULL;
	close(fd);
}


int
main(int argc, char** argv)
{
	printf("=== MI_BATCH_BUFFER_START Test (userspace clone) ===\n\n");

	if (!init_gpu()) return 1;
	printf("GPU OK, Gen %u\n",
		gInfo->shared_info->device_type.Generation());

	// Allocate two BOs: one for the batch, one for the marker.
	gpu_bo batch_bo, marker_bo;
	if (gpu_bo_alloc(&batch_bo, "test:batch", 4096, 4096) != B_OK) {
		printf("batch_bo alloc failed\n");
		cleanup_gpu(); return 1;
	}
	if (gpu_bo_alloc(&marker_bo, "test:marker", 4096, 64) != B_OK) {
		printf("marker_bo alloc failed\n");
		gpu_bo_free(&batch_bo);
		cleanup_gpu(); return 1;
	}

	printf("batch_bo:  cpu=%p gtt=0x%x\n",
		(void*)batch_bo.cpu_addr, batch_bo.gtt_offset);
	printf("marker_bo: cpu=%p gtt=0x%x\n",
		(void*)marker_bo.cpu_addr, marker_bo.gtt_offset);

	// === Test 1: MI_STORE_DATA_IMM via ring (direct, should work) ===
	printf("\n--- Test 1: MI_STORE_DATA_IMM direct in ring ---\n");
	volatile uint32* marker = (volatile uint32*)marker_bo.cpu_addr;
	*marker = 0;
	gpu_bo_flush_cpu_writes();

	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(4);
		queue.Write((0x20 << 23) | (1 << 22) | 2);  // MI_STORE_DATA_IMM|GGTT
		queue.Write(0);
		queue.Write(marker_bo.gtt_offset);
		queue.Write(0xCAFE0001);
	}

	// Busy-wait
	bigtime_t t0 = system_time();
	while (*marker != 0xCAFE0001 && (system_time() - t0) < 100000)
		;
	printf("  marker = 0x%08x %s\n", *marker,
		*marker == 0xCAFE0001 ? "PASS" : "FAIL");

	if (*marker != 0xCAFE0001) {
		printf("Ring direct submission broken — cannot proceed.\n");
		gpu_bo_free(&marker_bo);
		gpu_bo_free(&batch_bo);
		cleanup_gpu();
		return 1;
	}

	// === Test 2: MI_STORE_DATA_IMM via batch buffer ===
	printf("\n--- Test 2: MI_STORE_DATA_IMM via MI_BATCH_BUFFER_START ---\n");
	*marker = 0;

	// Write batch content: MI_STORE_DATA_IMM + MI_BATCH_BUFFER_END
	uint32* batch = (uint32*)batch_bo.cpu_addr;
	int bp = 0;
	batch[bp++] = (0x20 << 23) | (1 << 22) | 2;  // MI_STORE_DATA_IMM|GGTT
	batch[bp++] = 0;
	batch[bp++] = marker_bo.gtt_offset;
	batch[bp++] = 0xCAFE0002;
	batch[bp++] = MI_BATCH_BUFFER_END;
	if (bp & 1)
		batch[bp++] = MI_NOOP;

	gpu_bo_flush_cpu_writes();

	// Verify batch content via readback through graphics_memory
	volatile uint32* batch_gpu = (volatile uint32*)(
		(uint8*)gInfo->shared_info->graphics_memory + batch_bo.gtt_offset);
	printf("  batch: %d dwords at GTT 0x%x\n", bp, batch_bo.gtt_offset);
	printf("  batch via cpu_addr:   0x%08x 0x%08x 0x%08x 0x%08x\n",
		batch[0], batch[1], batch[2], batch[3]);
	printf("  batch via gfx_mem:    0x%08x 0x%08x 0x%08x 0x%08x\n",
		batch_gpu[0], batch_gpu[1], batch_gpu[2], batch_gpu[3]);

	// Try different MI_BATCH_BUFFER_START encodings
	// Gen5: DW0 = (0x31 << 23), DW1 = GTT address
	// Some gens need Address Space Indicator bit or length field
	printf("  MI_BATCH_BUFFER_START = 0x%08x\n", MI_BATCH_BUFFER_START);
	printf("  batch GTT = 0x%08x\n", batch_bo.gtt_offset);

	// Submit MI_BATCH_BUFFER_START via ring.
	// Gen4/5: bits [7:6] = address space: 00=physical, 10=GGTT.
	// We need MI_BATCH_GTT (2<<6) since batch_bo.gtt_offset is a GTT address.
	{
		ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
		QueueCommands queue(ring);
		queue.MakeSpace(2);
		queue.Write(MI_BATCH_BUFFER_START | MI_BATCH_GTT);
		queue.Write(batch_bo.gtt_offset);
	}

	// Busy-wait
	t0 = system_time();
	while (*marker != 0xCAFE0002 && (system_time() - t0) < 100000)
		;
	printf("  marker = 0x%08x %s\n", *marker,
		*marker == 0xCAFE0002 ? "PASS — BATCH BUFFER WORKS!" : "FAIL");

	if (*marker == 0xCAFE0002) {
		printf("\n>>> MI_BATCH_BUFFER_START works from userspace clone! <<<\n");
		printf(">>> This is the foundation for Mesa GEM_EXECBUFFER2. <<<\n");
	}

	gpu_bo_free(&marker_bo);
	gpu_bo_free(&batch_bo);
	cleanup_gpu();
	return 0;
}
