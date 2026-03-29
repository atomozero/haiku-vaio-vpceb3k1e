#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

int main() {
    int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
    if (fd < 0) { printf("Cannot open device\n"); return 1; }

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

    printf("=== State BEFORE ===\n");
    printf("  HEAD=0x%08x TAIL=0x%08x\n", R32(rb+4), R32(rb+0));
    printf("  CTL=0x%08x START=0x%08x\n", R32(rb+0xc), R32(rb+8));
    printf("  INSTDONE=0x%08x\n", R32(0x206c));

    // Try to find the physical address for START.
    // ring.base is the virtual address of the ring buffer.
    // The ring buffer lives in the GTT aperture.
    // Calculate GTT offset from the difference.
    ptrdiff_t ringGTTOffset = (uint8*)ring.base - (uint8*)si->graphics_memory;
    printf("  Ring GTT offset = 0x%lx (calculated from pointers)\n",
        (unsigned long)ringGTTOffset);
    printf("  Ring size = %d KB\n", ring.size / 1024);

    printf("\n=== Ring Reinit (no GPU reset) ===\n");
    acquire_lock(&ring.lock);

    // 1. Disable ring
    W32(rb + 0xc, 0);
    R32(rb + 0xc);
    snooze(2000);

    // 2. Reset HEAD and TAIL
    W32(rb + 0x04, 0);
    R32(rb + 0x04);
    snooze(1000);
    W32(rb + 0x00, 0);
    R32(rb + 0x00);
    snooze(1000);

    // 3. Write START register with correct GTT address
    // START register takes the base address >> 12 in bits 31:12
    // But actually on Gen5, START is just the physical address with bits 11:0 = 0
    uint32 startVal = (uint32)ringGTTOffset;
    printf("  Writing START = 0x%08x\n", startVal);
    W32(rb + 0x08, startVal);
    R32(rb + 0x08);
    snooze(1000);

    // 4. Clear ring memory
    memset((void*)ring.base, 0, ring.size);

    // 5. Re-enable ring
    uint32 ctlVal = ((ring.size - 4096) & 0x001ff000) | 1;
    printf("  Writing CTL = 0x%08x\n", ctlVal);
    W32(rb + 0xc, ctlVal);
    R32(rb + 0xc);
    snooze(5000);

    // 6. Reset software state
    ring.position = 0;
    ring.space_left = ring.size;

    release_lock(&ring.lock);

    printf("\n=== State AFTER ===\n");
    printf("  HEAD=0x%08x TAIL=0x%08x\n", R32(rb+4), R32(rb+0));
    printf("  CTL=0x%08x START=0x%08x\n", R32(rb+0xc), R32(rb+8));
    printf("  INSTDONE=0x%08x\n", R32(0x206c));

    // Test MI_STORE_DATA_IMM
    volatile uint32* scratch = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    *scratch = 0;
    int32 barrier = 0;
    atomic_add(&barrier, 1);

    acquire_lock(&ring.lock);
    uint32 pos = 0;
    *(uint32*)(ring.base + pos) = 0x10400002; pos += 4;
    *(uint32*)(ring.base + pos) = 0;          pos += 4;
    *(uint32*)(ring.base + pos) = 0x10000;    pos += 4;
    *(uint32*)(ring.base + pos) = 0xFACE0042; pos += 4;
    ring.position = pos;
    ring.space_left = ring.size - pos;
    atomic_add(&barrier, 1);
    W32(rb + 0x00, pos);
    release_lock(&ring.lock);

    printf("\n=== Command Test ===\n");
    printf("  TAIL written = 0x%x\n", pos);

    for (int w = 0; w < 40; w++) {
        snooze(5000);
        uint32 head = R32(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
        uint32 val = *scratch;
        printf("  [%3dms] HEAD=0x%04x scratch=0x%08x\n",
            (w+1)*5, head, val);
        if (val == 0xFACE0042) {
            printf("  [OK] GPU is alive!\n");
            break;
        }
        if (head == pos) {
            printf("  HEAD caught up to TAIL, but marker not written?\n");
            break;
        }
    }

    close(fd);
    return 0;
}
