/*
 * hang_diag - Dump ring contents and GPU error state at hang point
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

static int sFd = -1;
static intel_shared_info* sSI = NULL;
static volatile uint8* sRegs = NULL;

static bool init_gpu() {
	DIR* dir = opendir("/dev/graphics");
	if (!dir) return false;
	char path[256] = {};
	struct dirent* e;
	while ((e = readdir(dir)) != NULL)
		if (strncmp(e->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path), "/dev/graphics/%s", e->d_name);
			break;
		}
	closedir(dir);
	if (!path[0]) return false;
	sFd = open(path, B_READ_WRITE);
	if (sFd < 0) return false;
	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0)
		return false;
	area_id a1 = clone_area("hd_si", (void**)&sSI, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (a1 < 0) return false;
	area_id a2 = clone_area("hd_regs", (void**)&sRegs, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, sSI->registers_area);
	return a2 >= 0;
}

static uint32 rr(uint32 off) { return *(volatile uint32*)(sRegs + off); }

int main() {
	if (!init_gpu()) { printf("GPU init fail\n"); return 1; }

	ring_buffer& ring = sSI->primary_ring_buffer;
	uint32 head = rr(ring.register_base + RING_BUFFER_HEAD);
	uint32 tail = rr(ring.register_base + RING_BUFFER_TAIL);
	uint32 ctl  = rr(ring.register_base + RING_BUFFER_CONTROL);
	uint32 start= rr(ring.register_base);  // RING_BUFFER_START = base+0

	printf("=== GPU Hang Diagnostic ===\n\n");
	printf("Ring: HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x\n",
		head, tail, ctl, start);
	printf("  ring.base=%p ring.size=%u\n", (void*)ring.base, ring.size);

	// Error registers (Gen5/Ironlake)
	printf("\n--- Error Registers ---\n");
	printf("  INSTDONE:     0x%08x\n", rr(0x206c));
	printf("  INSTDONE1:    0x%08x\n", rr(0x207c));  // Gen5
	printf("  EIR:          0x%08x\n", rr(0x20b0));  // error identity
	printf("  EMR:          0x%08x\n", rr(0x20b4));  // error mask
	printf("  ESR:          0x%08x\n", rr(0x20b8));  // error status
	printf("  IPEIR:        0x%08x\n", rr(0x2064));  // instr parser err IP
	printf("  IPEHR:        0x%08x\n", rr(0x2068));  // instr parser err hdr
	printf("  INSTPS:       0x%08x\n", rr(0x2070));  // instr parser state
	printf("  ACTHD:        0x%08x\n", rr(0x2074));  // active head
	printf("  MI_MODE:      0x%08x\n", rr(0x209c));
	printf("  PGTBL_ER:     0x%08x\n", rr(0x2024));  // page table errors
	printf("  ILK_GDSR:     0x%08x\n", rr(0xca8));   // GPU debug status

	// Dump ring buffer contents around HEAD
	printf("\n--- Ring Contents (around HEAD=0x%x) ---\n",
		head & 0x1FFFF8);  // mask to ring offset
	uint32 headOff = head & 0x1FFFF8;
	uint32* ringMem = (uint32*)(ring.base);

	// Show 16 DWORDs before HEAD and 32 after
	int startDW = (int)(headOff / 4) - 16;
	if (startDW < 0) startDW = 0;
	int endDW = (int)(headOff / 4) + 32;
	if (endDW > (int)(ring.size / 4)) endDW = ring.size / 4;

	for (int i = startDW; i < endDW; i++) {
		uint32 off = i * 4;
		const char* marker = "";
		if (off == headOff) marker = " <-- HEAD";
		if (off == tail) marker = " <-- TAIL";
		printf("  [0x%04x] 0x%08x%s\n", off, ringMem[i], marker);
	}

	// Also dump SW ring.position area
	printf("\n--- Ring around SW position=0x%x ---\n", ring.position);
	startDW = (int)(ring.position / 4) - 8;
	if (startDW < 0) startDW = 0;
	endDW = (int)(ring.position / 4) + 8;
	if (endDW > (int)(ring.size / 4)) endDW = ring.size / 4;
	for (int i = startDW; i < endDW; i++) {
		uint32 off = i * 4;
		const char* marker = (off == ring.position) ? " <-- pos" : "";
		printf("  [0x%04x] 0x%08x%s\n", off, ringMem[i], marker);
	}

	return 0;
}
