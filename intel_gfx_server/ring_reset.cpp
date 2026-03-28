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
    #define W32(off, val) (*(volatile uint32*)(regs + (off)) = (val))

    ring_buffer& ring = si->primary_ring_buffer;
    uint32 rb = ring.register_base;

    printf("Before reset:\n");
    printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc));

    // Full ring reset (matching i915 init_ring_common)
    acquire_lock(&ring.lock);

    // 1. Disable ring
    W32(rb + 0xc, 0);
    R32(rb + 0xc);  // posting read
    snooze(1000);

    // 2. Reset HEAD
    W32(rb + 0x4, 0);
    R32(rb + 0x4);
    for (int i = 0; i < 100; i++) {
        if ((R32(rb + 0x4) & 0x001ffffc) == 0) break;
        snooze(100);
    }

    // 3. Reset TAIL
    W32(rb + 0x0, 0);

    // 4. Re-enable with existing START
    W32(rb + 0xc,
        ((ring.size - 4096) & 0x001ff000)
        | 1);  // enable

    // 5. Reset software tracking
    ring.position = 0;
    ring.space_left = ring.size;

    release_lock(&ring.lock);

    snooze(1000);

    printf("\nAfter reset:\n");
    printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc));

    // Now test: submit MI_STORE_DATA_IMM to scratch
    volatile uint32* scratch = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    *scratch = 0;
    int32 barrier = 0;
    atomic_add(&barrier, 1);

    acquire_lock(&ring.lock);
    uint32* cmd = (uint32*)(ring.base + ring.position);
    cmd[0] = 0x10400002;
    cmd[1] = 0;
    cmd[2] = 0x10000;
    cmd[3] = 0xCAFEBABE;
    ring.position = 16;
    ring.space_left -= 16;
    atomic_add(&barrier, 1);
    W32(rb + 0x0, ring.position);
    release_lock(&ring.lock);

    snooze(10000);
    atomic_add(&barrier, 1);

    uint32 val = *scratch;
    printf("\nMI_STORE_DATA_IMM test: scratch=0x%08x (%s)\n",
        val, val == 0xCAFEBABE ? "SUCCESS!" : "FAIL");

    printf("HEAD=0x%x TAIL=0x%x\n", R32(rb+4), R32(rb+0));

    close(fd);
    return 0;
}
