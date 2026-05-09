/*
 * poke_test — Test MMIO register access via /dev/misc/poke
 *
 * Maps the GPU MMIO BAR0 directly with the poke driver to bypass
 * the potentially broken clone_area mapping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include <Drivers.h>
#include <PCI.h>

// Poke driver definitions (from poke.h)
#define POKE_SIGNATURE 'wltp'

enum {
	POKE_PORT_READ = B_DEVICE_OP_CODES_END + 1,
	POKE_PORT_WRITE,
	POKE_PORT_INDEXED_READ,
	POKE_PORT_INDEXED_WRITE,
	POKE_PCI_READ_CONFIG,
	POKE_PCI_WRITE_CONFIG,
	POKE_GET_NTH_PCI_INFO,
	POKE_GET_PHYSICAL_ADDRESS,
	POKE_MAP_MEMORY,
	POKE_UNMAP_MEMORY
};

typedef struct {
	uint32		signature;
	uint8		index;
	pci_info*	info;
	status_t	status;
} pci_info_args;

typedef struct {
	uint32		signature;
	uint8		bus;
	uint8		device;
	uint8		function;
	uint8		size;
	uint8		offset;
	uint32		value;
} pci_io_args;

typedef struct {
	uint32		signature;
	area_id		area;
	const char*	name;
	phys_addr_t	physical_address;
	size_t		size;
	uint32		flags;
	uint32		protection;
	void*		address;
} mem_map_args;


static int poke_fd = -1;

static uint32
pci_read_config(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size)
{
	pci_io_args args;
	args.signature = POKE_SIGNATURE;
	args.bus = bus;
	args.device = dev;
	args.function = func;
	args.offset = offset;
	args.size = size;
	args.value = 0;
	ioctl(poke_fd, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	return args.value;
}


int
main(int, char**)
{
	poke_fd = open("/dev/misc/poke", O_RDWR);
	if (poke_fd < 0) {
		printf("Cannot open /dev/misc/poke\n");
		return 1;
	}
	printf("=== MMIO Register Test via poke driver ===\n\n");

	// GPU is at PCI 00:02.0, device ID 0x0046
	uint32 devid = pci_read_config(0, 2, 0, 0x02, 2);
	printf("PCI 00:02.0 device ID: 0x%04x\n", devid);
	if (devid != 0x0046) {
		printf("Not the expected GPU!\n");
		close(poke_fd);
		return 1;
	}

	// Read BAR0 (MMIO base) at PCI offset 0x10
	uint32 bar0 = pci_read_config(0, 2, 0, 0x10, 4);
	printf("BAR0 raw: 0x%08x\n", bar0);
	uint64 mmio_phys = bar0 & 0xFFFFFFF0;  // mask type bits
	printf("MMIO phys: 0x%lx\n", (unsigned long)mmio_phys);

	// Check BAR size: read BAR2 for 64-bit BAR
	uint32 bar1 = pci_read_config(0, 2, 0, 0x14, 4);
	printf("BAR1 (upper 32): 0x%08x\n", bar1);

	// Map MMIO via poke
	mem_map_args map;
	memset(&map, 0, sizeof(map));
	map.signature = POKE_SIGNATURE;
	map.name = "gpu_mmio_test";
	map.physical_address = mmio_phys;
	map.size = 0x100000;  // 1MB should be enough for ring registers
	map.flags = 0;
	map.protection = B_READ_AREA | B_WRITE_AREA;

	status_t st = ioctl(poke_fd, POKE_MAP_MEMORY, &map, sizeof(map));
	printf("POKE_MAP_MEMORY: ioctl returned %d, area=%d, address=%p\n",
		(int)st, (int)map.area, map.address);
	// Poke driver may return area_id as ioctl result
	if (map.area < 0 && st < 0) {
		printf("POKE_MAP_MEMORY failed\n");
		close(poke_fd);
		return 1;
	}
	if (map.area <= 0)
		map.area = st;  // ioctl returned area_id
	if (map.address == NULL) {
		// Try to get address from area info
		area_info ai;
		if (get_area_info(map.area, &ai) == B_OK)
			map.address = ai.address;
	}
	if (map.address == NULL) {
		printf("Could not get mapped address\n");
		close(poke_fd);
		return 1;
	}

	volatile uint32* mmio = (volatile uint32*)map.address;
	printf("MMIO mapped at %p (area %d)\n\n", mmio, (int)map.area);

	// Ring registers at offset 0x2030 (ILK render ring)
	// Access as array: mmio[offset/4]
	uint32 tail = mmio[0x2030 / 4];
	uint32 head = mmio[0x2034 / 4] & 0x001FFFFC;
	uint32 start = mmio[0x2038 / 4];
	uint32 ctl = mmio[0x203C / 4];
	printf("Ring via poke:\n");
	printf("  TAIL=0x%x HEAD=0x%x START=0x%x CTL=0x%x\n",
		tail, head, start, ctl);

	// Test: write scratch register 0x2094 (MI_PREDICATE_SRC0)
	uint32 scratch_before = mmio[0x2094 / 4];
	mmio[0x2094 / 4] = 0xDEADBEEF;
	asm volatile("mfence" ::: "memory");
	uint32 scratch_after = mmio[0x2094 / 4];
	printf("\nScratch test (0x2094):\n");
	printf("  before=0x%x, wrote 0xDEADBEEF, read=0x%x\n",
		scratch_before, scratch_after);

	// Test: try writing TAIL directly
	printf("\nTAIL write test:\n");
	uint32 old_tail = mmio[0x2030 / 4];
	printf("  TAIL before: 0x%x\n", old_tail);
	mmio[0x2030 / 4] = old_tail;  // write same value back
	asm volatile("mfence" ::: "memory");
	uint32 new_tail = mmio[0x2030 / 4];
	printf("  TAIL after write-back: 0x%x\n", new_tail);

	// Check HEAD after a moment
	snooze(10000);
	uint32 head2 = mmio[0x2034 / 4] & 0x001FFFFC;
	printf("  HEAD after 10ms: 0x%x (was 0x%x)\n", head2, head);

	// Cleanup
	mem_map_args unmap;
	unmap.signature = POKE_SIGNATURE;
	unmap.area = map.area;
	ioctl(poke_fd, POKE_UNMAP_MEMORY, &unmap, sizeof(unmap));
	close(poke_fd);

	printf("\nDone.\n");
	return 0;
}
