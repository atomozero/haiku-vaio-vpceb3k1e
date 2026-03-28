#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

int main() {
    // Open device
    int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
    if (fd < 0) { printf("Cannot open device\n"); return 1; }

    intel_get_private_data data;
    data.magic = INTEL_PRIVATE_DATA_MAGIC;
    ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));

    intel_shared_info* si;
    clone_area("test_si", (void**)&si, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, data.shared_info_area);

    volatile uint8* regs;
    clone_area("test_regs", (void**)&regs, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, si->registers_area);

    #define R32(off) (*(volatile uint32*)(regs + (off)))

    ring_buffer& ring = si->primary_ring_buffer;

    printf("register_base = 0x%x\n", ring.register_base);
    printf("ring.position = %u (0x%x)\n", ring.position, ring.position);
    printf("ring.offset   = 0x%x\n", ring.offset);
    printf("ring.size     = %u\n", ring.size);
    printf("ring.base     = %p\n", (void*)ring.base);
    printf("HW TAIL = 0x%08x\n", R32(ring.register_base + 0x0));
    printf("HW HEAD = 0x%08x\n", R32(ring.register_base + 0x4));
    printf("HW START= 0x%08x\n", R32(ring.register_base + 0x8));
    printf("HW CTL  = 0x%08x\n", R32(ring.register_base + 0xc));

    // Check if software and hardware TAIL match
    printf("\nSW position (0x%x) vs HW TAIL (0x%x): %s\n",
        ring.position,
        R32(ring.register_base + 0x0),
        ring.position == R32(ring.register_base + 0x0) ? "MATCH" : "MISMATCH!");

    close(fd);
    return 0;
}
