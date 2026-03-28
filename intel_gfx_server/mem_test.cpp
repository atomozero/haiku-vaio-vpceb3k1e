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

    printf("graphics_memory = %p\n", (void*)si->graphics_memory);
    printf("frame_buffer    = %p\n", (void*)si->frame_buffer);
    printf("fb_offset       = 0x%x\n", si->frame_buffer_offset);

    // Test 1: can we read framebuffer via si->frame_buffer?
    uint32* fb = (uint32*)si->frame_buffer;
    uint32 stride = si->bytes_per_row / 4;
    printf("\nTest 1: Read pixel (400,70) via frame_buffer ptr\n");
    uint32 pix = *(volatile uint32*)&fb[70 * stride + 400];
    printf("  pixel = 0x%08x\n", pix);

    // Test 2: Write pixel via CPU, verify it changes
    printf("\nTest 2: CPU write pixel (500,10) = magenta\n");
    uint32 pixBefore = fb[10 * stride + 500];
    fb[10 * stride + 500] = 0xFFFF00FF;  // magenta
    int32 flush = 0;
    atomic_add(&flush, 1);
    uint32 pixAfter = *(volatile uint32*)&fb[10 * stride + 500];
    printf("  before=0x%08x after=0x%08x (%s)\n",
        pixBefore, pixAfter,
        pixAfter == 0xFFFF00FF ? "CPU WRITE OK" : "FAIL");

    // Test 3: Write to scratch (GTT 0x10000) via graphics_memory ptr
    printf("\nTest 3: Write scratch at GTT 0x10000 via graphics_memory\n");
    volatile uint32* scratch = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    printf("  graphics_memory + 0x10000 = %p\n", (void*)scratch);
    
    // Try to read first (might crash if not mapped!)
    printf("  Attempting read...");
    fflush(stdout);
    uint32 scratchVal = *scratch;
    printf(" OK (value=0x%08x)\n", scratchVal);
    
    // Write marker
    *scratch = 0xCAFEBABE;
    atomic_add(&flush, 1);
    uint32 scratchRead = *scratch;
    printf("  After write: 0x%08x (%s)\n",
        scratchRead,
        scratchRead == 0xCAFEBABE ? "SCRATCH WRITE OK" : "FAIL");

    // Test 4: Read ring buffer memory
    printf("\nTest 4: Ring buffer memory access\n");
    ring_buffer& ring = si->primary_ring_buffer;
    printf("  ring.base = %p\n", (void*)ring.base);
    volatile uint32* ringMem = (volatile uint32*)ring.base;
    printf("  Attempting read...");
    fflush(stdout);
    uint32 ringVal = *ringMem;
    printf(" OK (first DWORD=0x%08x)\n", ringVal);

    close(fd);
    return 0;
}
