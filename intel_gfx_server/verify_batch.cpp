#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
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

    // Allocate via kernel ioctl (same as GemManager)
    intel_allocate_graphics_memory alloc;
    alloc.magic = INTEL_PRIVATE_DATA_MAGIC;
    alloc.size = 4096;
    alloc.alignment = 0;
    alloc.flags = 0;
    if (ioctl(fd, INTEL_ALLOCATE_GRAPHICS_MEMORY, &alloc, sizeof(alloc)) < 0) {
        printf("Alloc failed!\n");
        return 1;
    }

    addr_t base = alloc.buffer_base;
    uint32 gttOff = base - (addr_t)si->graphics_memory;
    printf("Allocated: base=%p, GTT offset=0x%x\n", (void*)base, gttOff);

    // Write test data
    uint32* buf = (uint32*)base;
    buf[0] = 0xAABBCCDD;
    buf[1] = 0x11223344;
    __asm__ __volatile__("sfence" ::: "memory");

    // Read back via same pointer
    printf("Direct read:  [0]=0x%08x [1]=0x%08x\n",
        *(volatile uint32*)&buf[0], *(volatile uint32*)&buf[1]);

    // Read back via graphics_memory + offset
    volatile uint32* alt = (volatile uint32*)(
        (uint8*)si->graphics_memory + gttOff);
    printf("Via gfx_mem:  [0]=0x%08x [1]=0x%08x\n", alt[0], alt[1]);

    // Now write a batch and try MI_STORE_DATA_IMM to GTT 0x10000
    // to see if BATCH from this address works
    memset(buf, 0, 4096);
    buf[0] = 0x10400002;  // MI_STORE_DATA_IMM | GGTT
    buf[1] = 0;
    buf[2] = 0x10000;     // target: scratch
    buf[3] = 0xFACEFACE;
    buf[4] = 0x05000000;  // MI_BATCH_BUFFER_END
    buf[5] = 0;
    __asm__ __volatile__("sfence" ::: "memory");

    printf("\nBatch at GTT 0x%x:\n", gttOff);
    for (int i = 0; i < 6; i++)
        printf("  [%d] 0x%08x\n", i, *(volatile uint32*)&buf[i]);

    // Clear scratch
    volatile uint32* scratch = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    *scratch = 0;
    __asm__ __volatile__("sfence" ::: "memory");

    printf("\nScratch before: 0x%08x\n", *scratch);

    // Submit via ring (use lock!)
    volatile uint8* regs;
    clone_area("r", (void**)&regs, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, si->registers_area);
    ring_buffer& ring = si->primary_ring_buffer;

    acquire_lock(&ring.lock);
    uint32* cmd = (uint32*)(ring.base + ring.position);
    // Try WITHOUT NON_SECURE bit (like Haiku accelerant does)
    cmd[0] = 0x18800000;  // MI_BATCH_BUFFER_START (plain, no NON_SECURE)
    cmd[1] = gttOff;
    cmd[2] = 0x02000000;  // MI_FLUSH
    cmd[3] = 0;            // MI_NOOP
    ring.position = (ring.position + 16) & (ring.size - 1);
    ring.space_left -= 16;
    __asm__ __volatile__("sfence" ::: "memory");
    *(volatile uint32*)(regs + ring.register_base) = ring.position;
    release_lock(&ring.lock);

    snooze(50000);
    __asm__ __volatile__("lfence" ::: "memory");

    uint32 scratchVal = *scratch;
    uint32 head = *(volatile uint32*)(regs + ring.register_base + 4) & 0x1ffffc;
    uint32 tail = *(volatile uint32*)(regs + ring.register_base);
    printf("Scratch after:  0x%08x (%s)\n", scratchVal,
        scratchVal == 0xFACEFACE ? "BATCH WORKS!" : "FAIL");
    printf("HEAD=0x%x TAIL=0x%x\n", head, tail);

    close(fd);
    return scratchVal == 0xFACEFACE ? 0 : 1;
}
