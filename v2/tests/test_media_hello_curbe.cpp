/*
 * test_media_hello_curbe.cpp — Test B: hello_world with CURBE push.
 * Same EOT kernel but curbe_read_len=20, 10-unit CONSTANT_BUFFER.
 * If this hangs, the CURBE loading is the problem.
 */

#include "gpu_ring.h"
#include "intel_extreme.h"
#include "idct_ref.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <OS.h>

static const uint32 kHelloKernel[][4] = {
#include "../../intel_extreme/accelerant/kernels/hello_world.g4b.gen5"
};

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

static int sFd = -1;
static uint8* sGfx = NULL;

static addr_t galloc(size_t sz, size_t al) {
	intel_allocate_graphics_memory a;
	a.magic = INTEL_PRIVATE_DATA_MAGIC; a.size = sz; a.alignment = al; a.flags = 0;
	return (ioctl(sFd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &a, sizeof(a)) < 0) ? 0 : a.buffer_base;
}
static uint8* gcpu(addr_t b) { return sGfx + (b - (addr_t)sGfx); }
static uint32 goff(addr_t b) { return (uint32)(b - (addr_t)sGfx); }

int main()
{
	printf("=== Test B: Hello + CURBE (curbe_read_len=20) ===\n\n");

	sFd = open("/dev/graphics/intel_extreme_000200", O_RDWR);
	if (sFd < 0) sFd = open("/dev/graphics/intel_extreme/0", O_RDWR);
	if (sFd < 0) { printf("Cannot open device\n"); return 1; }

	intel_get_private_data p; p.magic = INTEL_PRIVATE_DATA_MAGIC;
	ioctl(sFd, INTEL_GET_PRIVATE_DATA, &p, sizeof(p));
	intel_shared_info* sh;
	clone_area("test shared", (void**)&sh, B_ANY_ADDRESS, B_READ_AREA, p.shared_info_area);
	sGfx = (uint8*)sh->graphics_memory;

	// Apply Gen5 workarounds (MI_FLUSH fix)
	{
		intel_get_private_data init3d;
		init3d.magic = INTEL_PRIVATE_DATA_MAGIC;
		if (ioctl(sFd, INTEL_RING_INIT_3D, &init3d, sizeof(init3d)) == 0)
			printf("3D workarounds applied\n");
		else
			printf("WARNING: INTEL_RING_INIT_3D failed\n");
	}

	gpu_ring ring;
	if (gpu_ring_init(&ring, sFd) != B_OK) { printf("ring init failed\n"); return 1; }

	addr_t kernel_base = galloc(4096, 64);
	addr_t vfe_base    = galloc(64, 64);
	addr_t idrt_base   = galloc(64, 64);
	addr_t curbe_base  = galloc(1024, 64);
	addr_t marker_base = galloc(64, 64);
	if (!kernel_base || !vfe_base || !idrt_base || !curbe_base || !marker_base) {
		printf("alloc failed\n"); return 1;
	}

	// Kernel
	memset(gcpu(kernel_base), 0, 4096);
	memcpy(gcpu(kernel_base), kHelloKernel, sizeof(kHelloKernel));

	// CURBE: 20 GRFs = 640 bytes. g1-g4 padding, g5-g20 cosine table.
	memset(gcpu(curbe_base), 0, 1024);
	memcpy(gcpu(curbe_base) + 128, kIdctTableGpu, sizeof(kIdctTableGpu));

	// VFE: 1 thread
	memset(gcpu(vfe_base), 0, 64);
	uint32* vfe = (uint32*)gcpu(vfe_base);
	vfe[0] = 0;
	vfe[1] = (0 << 3) | (1 << 9) | (0 << 16) | (0 << 25);
	vfe[2] = goff(idrt_base) & ~0xFu;

	// IDRT: curbe_read_len=20, grf_reg_blocks=15, no binding table
	memset(gcpu(idrt_base), 0, 64);
	uint32* idrt = (uint32*)gcpu(idrt_base);
	idrt[0] = 15u | (goff(kernel_base) & ~0x3Fu);
	idrt[1] = (1u << 18) | (20u << 26);  // single_program_flow + curbe_read_len=20
	idrt[2] = 0;
	idrt[3] = 0;  // no binding table

	*(volatile uint32*)gcpu(marker_base) = MARKER_SENTINEL;
	asm volatile("mfence" ::: "memory");

	printf("IDRT: DW0=0x%x DW1=0x%x (curbe_read_len=%u)\n",
		idrt[0], idrt[1], (idrt[1] >> 26) & 0x3f);
	printf("CURBE: gtt=0x%x, first 4 bytes at offset 128: 0x%x\n",
		goff(curbe_base), *(uint32*)(gcpu(curbe_base) + 128));

	uint32 head_before = gpu_ring_read_head(&ring);
	printf("Submitting (HEAD=0x%x)...\n", head_before);

	gpu_ring_begin(&ring, 64);
	gpu_ring_emit(&ring, MI_FLUSH);
	gpu_ring_emit(&ring, CMD_3DSTATE_DEPTH_BUFFER);
	gpu_ring_emit(&ring, (7u << 29) | (1u << 18));
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gpu_ring_emit(&ring, CMD_STATE_BASE_ADDRESS | 6);
	for (int j = 0; j < 7; j++) gpu_ring_emit(&ring, BASE_ADDRESS_MODIFY);
	gpu_ring_emit(&ring, CMD_URB_FENCE | UF0_VFE_REALLOC | UF0_CS_REALLOC | 1);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, (1u << 10) | (17u << 20));
	gpu_ring_emit(&ring, CMD_MEDIA_STATE_POINTERS | 1);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, goff(vfe_base));
	gpu_ring_emit(&ring, CMD_CS_URB_STATE | 0);
	gpu_ring_emit(&ring, ((16 - 1) << 4) | 1);
	// CONSTANT_BUFFER: 10 units = 640 bytes = 20 GRFs
	gpu_ring_emit(&ring, CMD_CONSTANT_BUFFER | (1u << 8) | 0);
	gpu_ring_emit(&ring, goff(curbe_base) | ((10 - 1) & 0x1f));

	// MEDIA_OBJECT: 6 DW (no inline beyond minimum)
	gpu_ring_emit(&ring, CMD_MEDIA_OBJECT | 4);
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, 0); gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, 0);

	// Marker
	gpu_ring_emit(&ring, MI_STORE_DATA_IMM_GGTT);
	gpu_ring_emit(&ring, 0);
	gpu_ring_emit(&ring, goff(marker_base));
	gpu_ring_emit(&ring, 0xBEEF00B0);
	gpu_ring_emit(&ring, MI_FLUSH);
	gpu_ring_advance(&ring);

	bigtime_t deadline = system_time() + 500000;
	bool ok = false;
	while (system_time() < deadline) {
		if (*(volatile uint32*)gcpu(marker_base) == 0xBEEF00B0) { ok = true; break; }
		snooze(50);
	}

	uint32 head_after = gpu_ring_read_head(&ring);
	printf("Result: HEAD 0x%x → 0x%x, marker=0x%x %s\n",
		head_before, head_after,
		*(volatile uint32*)gcpu(marker_base),
		ok ? "CURBE TEST OK!" : "CURBE TEST HANG");

	gpu_ring_wait_idle(&ring, 100000);
	close(sFd);
	return ok ? 0 : 1;
}
