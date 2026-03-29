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
    clone_area("t", (void**)&si, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
    volatile uint8* regs;
    clone_area("t_r", (void**)&regs, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, si->registers_area);

    #define R32(off) (*(volatile uint32*)(regs + (off)))

    ring_buffer& ring = si->primary_ring_buffer;
    uint32 rb = ring.register_base;

    printf("Ring: HEAD=0x%08x TAIL=0x%08x CTL=0x%08x START=0x%08x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc), R32(rb+8));
    printf("SW:   pos=0x%x space=%u/%u\n",
        ring.position, ring.space_left, ring.size);
    printf("INSTDONE=0x%08x\n", R32(0x206c));
    printf("HEAD==TAIL? %s\n",
        (R32(rb+4) & 0x1ffffc) == R32(rb+0) ? "YES" : "NO");
    close(fd);
    return 0;
}
