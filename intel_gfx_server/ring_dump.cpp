#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

int main() {
    int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
    intel_get_private_data data;
    data.magic = INTEL_PRIVATE_DATA_MAGIC;
    ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));

    intel_shared_info* si;
    clone_area("t_si", (void**)&si, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, data.shared_info_area);

    volatile uint8* regs;
    clone_area("t_regs", (void**)&regs, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, si->registers_area);

    #define R32(off) (*(volatile uint32*)(regs + (off)))

    ring_buffer& ring = si->primary_ring_buffer;
    uint32 head = R32(ring.register_base + 0x4) & INTEL_RING_BUFFER_HEAD_MASK;
    uint32 tail = R32(ring.register_base + 0x0);

    printf("HEAD=0x%x  TAIL=0x%x  SW_pos=0x%x\n\n", head, tail, ring.position);

    // Dump ring contents from before HEAD to after TAIL
    uint32 dumpStart = (head >= 0x40) ? head - 0x40 : 0;
    uint32 dumpEnd = (tail + 0x40 < ring.size) ? tail + 0x40 : ring.size;

    printf("Ring buffer dump (0x%x - 0x%x):\n", dumpStart, dumpEnd);
    uint32* ringMem = (uint32*)ring.base;
    for (uint32 off = dumpStart; off < dumpEnd; off += 4) {
        uint32 val = ringMem[off / 4];
        const char* marker = "";
        if (off == head) marker = " <-- HEAD (GPU stopped here)";
        else if (off == tail) marker = " <-- TAIL";
        printf("  [0x%04x] 0x%08x%s\n", off, val, marker);
    }

    close(fd);
    return 0;
}
