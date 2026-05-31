/*
 * test_media_hello.cpp — Minimal media pipeline test.
 *
 * Dispatches hello_world kernel (1 instruction: EOT) via the media
 * pipeline to verify the preamble works. No surfaces, no CURBE.
 * If this hangs, the preamble is wrong. If it works, the issue
 * is in the IDCT kernel or its surface state setup.
 *
 * Build:
 *   g++ -Wall -O2 -I../../intel_extreme/accelerant \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics/common \
 *       -I/boot/system/develop/headers/os/add-ons/graphics \
 *       -I/boot/system/develop/headers/os/drivers \
 *       -I/boot/system/develop/headers/private/graphics \
 *       -I/boot/system/develop/headers/private/shared \
 *       -o test_media_hello test_media_hello.cpp \
 *       ../../intel_extreme/accelerant/gpu_ring.cpp -lbe -lstdc++
 */

#include "gpu_ring.h"
#include "intel_extreme.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <OS.h>


/* Hello world kernel: just EOT (1 instruction) */
static const uint32 kHelloKernel[][4] = {
#include "../../intel_extreme/accelerant/kernels/hello_world.g4b.gen5"
};

/* Command opcodes */
#define CMD_PIPELINE_SELECT      0x69040000
#define PIPELINE_SELECT_MEDIA    0x00000001
#define CMD_STATE_BASE_ADDRESS   0x61010000
#define CMD_URB_FENCE            0x60000000
#define CMD_MEDIA_STATE_POINTERS 0x70000000
#define CMD_CS_URB_STATE         0x60010000
#define CMD_CONSTANT_BUFFER      0x60020000
#define CMD_MEDIA_OBJECT         0x71000000
#define CMD_3DSTATE_DEPTH_BUFFER 0x79050005
#define BASE_ADDRESS_MODIFY      0x00000001
#define UF0_VFE_REALLOC          (1 << 12)
#define UF0_CS_REALLOC           (1 << 13)
#define MI_STORE_DATA_IMM_GGTT   0x10400002
#define MARKER_SENTINEL          0xDEADBEEFu
#define MARKER_TAG               0xBEEF0099u


static int sDeviceFd = -1;
static uint8* sGfxMem = NULL;


static addr_t
alloc_gtt(size_t size, size_t alignment)
{
	intel_allocate_graphics_memory alloc;
	alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
	alloc.size = size;
	alloc.alignment = alignment;
	alloc.flags = 0;
	if (ioctl(sDeviceFd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &alloc, sizeof(alloc)) < 0)
		return 0;
	return alloc.buffer_base;
}

static inline uint8* gtt_cpu(addr_t base) { return sGfxMem + (base - (addr_t)sGfxMem); }
static inline uint32 gtt_off(addr_t base) { return (uint32)(base - (addr_t)sGfxMem); }


int
main()
{
	printf("=== Media Pipeline Hello World Test ===\n\n");

	/* Open device */
	sDeviceFd = open("/dev/graphics/intel_extreme_000200", O_RDWR);
	if (sDeviceFd < 0) sDeviceFd = open("/dev/graphics/intel_extreme/0", O_RDWR);
	if (sDeviceFd < 0) { printf("Cannot open device\n"); return 1; }

	/* Get graphics memory */
	intel_get_private_data priv;
	priv.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(sDeviceFd, INTEL_GET_PRIVATE_DATA, &priv, sizeof(priv));
	intel_shared_info* shared;
	clone_area("hello shared", (void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA, priv.shared_info_area);
	sGfxMem = (uint8*)shared->graphics_memory;

	/* Init ring */
	gpu_ring ring;
	if (gpu_ring_init(&ring, sDeviceFd) != B_OK) {
		printf("gpu_ring_init failed\n"); return 1;
	}
	printf("Ring: pos=0x%x size=%u\n", ring.pos, ring.size);

	/* Allocate minimal GTT buffers */
	addr_t kernel_base = alloc_gtt(4096, 64);
	addr_t vfe_base = alloc_gtt(64, 64);
	addr_t idrt_base = alloc_gtt(64, 64);
	addr_t marker_base = alloc_gtt(64, 64);
	if (!kernel_base || !vfe_base || !idrt_base || !marker_base) {
		printf("GTT alloc failed\n"); return 1;
	}

	/* Upload hello_world kernel */
	memset(gtt_cpu(kernel_base), 0, 4096);
	memcpy(gtt_cpu(kernel_base), kHelloKernel, sizeof(kHelloKernel));

	/* VFE state: 1 thread, no URB, generic mode */
	memset(gtt_cpu(vfe_base), 0, 64);
	uint32* vfe = (uint32*)gtt_cpu(vfe_base);
	vfe[0] = 0;  /* no scratch */
	vfe[1] = (0 << 3) | (1 << 9) | (0 << 16) | (0 << 25);  /* 1 URB, max=0(=1) */
	vfe[2] = gtt_off(idrt_base) & ~0xFu;  /* IDRT pointer */

	/* IDRT: hello kernel, no CURBE, no binding table */
	memset(gtt_cpu(idrt_base), 0, 64);
	uint32* idrt = (uint32*)gtt_cpu(idrt_base);
	idrt[0] = 2u | (gtt_off(kernel_base) & ~0x3Fu);  /* grf_reg_blocks=2(=24 regs), kernel ptr */
	idrt[1] = (1u << 18);  /* single_program_flow=1, curbe_read_len=0 */
	idrt[2] = 0;  /* no sampler */
	idrt[3] = 0;  /* no binding table */

	/* Reset marker */
	*(volatile uint32*)gtt_cpu(marker_base) = MARKER_SENTINEL;
	asm volatile("mfence" ::: "memory");

	printf("GTT: kernel=0x%x vfe=0x%x idrt=0x%x marker=0x%x\n",
		gtt_off(kernel_base), gtt_off(vfe_base),
		gtt_off(idrt_base), gtt_off(marker_base));
	printf("VFE: DW0=0x%x DW1=0x%x DW2=0x%x\n", vfe[0], vfe[1], vfe[2]);
	printf("IDRT: DW0=0x%x DW1=0x%x DW2=0x%x DW3=0x%x\n",
		idrt[0], idrt[1], idrt[2], idrt[3]);

	/* Submit media preamble + 1 MEDIA_OBJECT + marker directly in ring */
	uint32 head_before = gpu_ring_read_head(&ring);
	printf("\nSubmitting media pipeline (HEAD=0x%x)...\n", head_before);

	status_t st = gpu_ring_begin(&ring, 64);
	if (st != B_OK) { printf("begin failed\n"); return 1; }

	/* Preamble */
	gpu_ring_emit(&ring, MI_FLUSH);
	gpu_ring_emit(&ring, CMD_3DSTATE_DEPTH_BUFFER);
	gpu_ring_emit(&ring, (7u << 29) | (1u << 18));
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gpu_ring_emit(&ring, CMD_STATE_BASE_ADDRESS | 6);
	for (int j = 0; j < 7; j++)
		gpu_ring_emit(&ring, BASE_ADDRESS_MODIFY);
	gpu_ring_emit(&ring, CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, (1u << 10) | (17u << 20));  /* 1 VFE entry, CS fence at 17 */
	gpu_ring_emit(&ring, CMD_MEDIA_STATE_POINTERS | 1);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, gtt_off(vfe_base));
	gpu_ring_emit(&ring, CMD_CS_URB_STATE | 0);
	gpu_ring_emit(&ring, ((16 - 1) << 4) | 1);
	gpu_ring_emit(&ring, CMD_CONSTANT_BUFFER | (1u << 8) | 0);
	gpu_ring_emit(&ring, gtt_off(vfe_base) | 0);  /* dummy, len=1 unit */

	/* MEDIA_OBJECT: 1 dispatch, no inline data (6 DW) */
	gpu_ring_emit(&ring, CMD_MEDIA_OBJECT | 4);  /* 4+2=6 DW */
	gpu_ring_emit(&ring, 0);  /* interface descriptor 0 */
	gpu_ring_emit(&ring, 0);  /* no indirect */
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, 0);  /* inline DW0 */
	gpu_ring_emit(&ring, 0);  /* inline DW1 */

	/* Marker */
	gpu_ring_emit(&ring, MI_STORE_DATA_IMM_GGTT);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, gtt_off(marker_base));
	gpu_ring_emit(&ring, MARKER_TAG);

	/* Flush */
	gpu_ring_emit(&ring, MI_FLUSH);

	st = gpu_ring_advance(&ring);
	if (st != B_OK) { printf("advance failed\n"); return 1; }

	/* Wait for marker */
	bigtime_t deadline = system_time() + 500000;
	bool ok = false;
	while (system_time() < deadline) {
		if (*(volatile uint32*)gtt_cpu(marker_base) == MARKER_TAG) {
			ok = true;
			break;
		}
		snooze(50);
	}

	uint32 head_after = gpu_ring_read_head(&ring);
	uint32 marker_val = *(volatile uint32*)gtt_cpu(marker_base);

	printf("Result: HEAD 0x%x → 0x%x, marker=0x%x %s\n",
		head_before, head_after, marker_val,
		ok ? "MEDIA PIPELINE OK!" : "MEDIA PIPELINE HANG");

	close(sDeviceFd);
	return ok ? 0 : 1;
}
