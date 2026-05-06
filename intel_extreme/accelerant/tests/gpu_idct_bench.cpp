/*
 * gpu_idct_bench — Standalone GPU IDCT benchmark (no reboot needed).
 *
 * Opens the intel_extreme device, maps shared_info to get ring buffer
 * access, then calls media_pipeline_run_mc_bench() which does the
 * full IDCT benchmark internally (400 blocks GPU vs CPU).
 *
 * Build (from accelerant/ dir after 'make'):
 *   cd tests && g++ -Wall -O2 \
 *       -I.. \
 *       -I/boot/system/develop/headers/os/add-ons/graphics \
 *       -I/boot/system/develop/headers/os/drivers \
 *       -I/boot/system/develop/headers/private/graphics \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics/common \
 *       -I/boot/system/develop/headers/private/shared \
 *       -I/boot/system/develop/headers/private/system \
 *       -I/boot/system/develop/headers/private/kernel/boot \
 *       -I/boot/system/develop/headers/os \
 *       -I/boot/system/develop/headers/os/support \
 *       -I/boot/system/develop/headers/os/interface \
 *       -I/boot/system/develop/headers/os/kernel \
 *       -I/boot/system/develop/headers/os/storage \
 *       -I/boot/system/develop/headers/os/app \
 *       -I/boot/system/develop/headers/posix \
 *       -o gpu_idct_bench gpu_idct_bench.cpp \
 *       ../*.o ../../libaccelerantscommon.a \
 *       -lbe -lstdc++
 *
 * Run:
 *   ./gpu_idct_bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <OS.h>
#include <image.h>

#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "media_pipeline.h"

// The accelerant's global state (defined in accelerant.cpp).
extern accelerant_info* gInfo;

// media_pipeline_run_mc_bench is declared in media_pipeline.h
// (it currently does the IDCT benchmark despite the name)


int
main(int argc, char** argv)
{
	printf("=== GPU IDCT Benchmark (userspace, no reboot) ===\n\n");

	// 1. Open device.
	int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (fd < 0) {
		printf("Cannot open /dev/graphics/intel_extreme_000200: %s\n",
			strerror(errno));
		return 1;
	}

	// 2. Get shared_info area.
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		printf("INTEL_GET_PRIVATE_DATA ioctl failed: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	// 3. Set up gInfo (same as init_common with isClone=true).
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL) {
		close(fd);
		return 1;
	}
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = true;
	gInfo->device = fd;

	// 4. Map shared_info.
	gInfo->shared_info_area = clone_area("bench shared info",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (gInfo->shared_info_area < B_OK) {
		printf("clone shared_info failed: %s\n",
			strerror(gInfo->shared_info_area));
		free(gInfo);
		close(fd);
		return 1;
	}

	// 5. Map registers.
	gInfo->regs_area = clone_area("bench regs",
		(void**)&gInfo->registers, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area);
	if (gInfo->regs_area < B_OK) {
		printf("clone registers failed: %s\n",
			strerror(gInfo->regs_area));
		delete_area(gInfo->shared_info_area);
		free(gInfo);
		close(fd);
		return 1;
	}

	printf("GPU access OK\n");
	printf("  device_type = 0x%04x (Gen %u)\n",
		gInfo->shared_info->device_type.type,
		gInfo->shared_info->device_type.Generation());
	printf("  graphics_memory = %p\n",
		gInfo->shared_info->graphics_memory);
	printf("  ring: size=%u pos=%u\n",
		gInfo->shared_info->primary_ring_buffer.size,
		gInfo->shared_info->primary_ring_buffer.position);

	// 6. Run the benchmark (all internals handled by media_pipeline).
	printf("\nRunning IDCT benchmark...\n\n");
	status_t st = media_pipeline_run_mc_bench();
	printf("\nBenchmark %s (status=%d)\n",
		st == B_OK ? "COMPLETED" : "FAILED", (int)st);

	// 7. Cleanup.
	delete_area(gInfo->regs_area);
	delete_area(gInfo->shared_info_area);
	free(gInfo);
	gInfo = NULL;
	close(fd);
	return st == B_OK ? 0 : 1;
}
