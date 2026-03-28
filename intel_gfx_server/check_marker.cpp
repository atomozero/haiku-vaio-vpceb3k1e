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

    // Check if PIPE_CONTROL marker was written to GTT 0x20000
    volatile uint32* at20000 = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x20000);
    printf("GTT 0x20000: 0x%08x %s\n", *at20000,
        *at20000 == 0xdeadbeef ? "<-- DEADBEEF FOUND!" : "");
    printf("GTT 0x20004: 0x%08x\n", *(at20000 + 1));

    // Also check GTT 0x10000 (our scratch test target)
    volatile uint32* at10000 = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    printf("GTT 0x10000: 0x%08x\n", *at10000);

    // And the ring buffer content at HEAD to see why GPU stalled
    volatile uint8* regs;
    clone_area("t_r", (void**)&regs, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, si->registers_area);
    uint32 head = *(volatile uint32*)(regs + 0x2034) & 0x001ffffc;
    uint32 tail = *(volatile uint32*)(regs + 0x2030);
    uint32 ipeir = *(volatile uint32*)(regs + 0x2064);
    uint32 ipehr = *(volatile uint32*)(regs + 0x2068);
    uint32 eir = *(volatile uint32*)(regs + 0x20b0);
    printf("\nHEAD=0x%x TAIL=0x%x IPEIR=0x%x IPEHR=0x%x EIR=0x%x\n",
        head, tail, ipeir, ipehr, eir);

    close(fd);
    return 0;
}
