#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

// Ironlake GPU reset register (from i915)
#define ILK_GDSR            0x2ca4
#define ILK_GRDOM_RENDER     (1 << 0)
#define ILK_GRDOM_MEDIA      (3 << 0)
#define ILK_GRDOM_RESET_EN   (1 << 31)

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

    printf("=== Pre-reset state ===\n");
    printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc), R32(rb+8));
    printf("  ILK_GDSR=0x%08x\n", R32(ILK_GDSR));

    // Step 1: Disable ring first
    printf("\n=== Step 1: Disable ring ===\n");
    acquire_lock(&ring.lock);
    W32(rb + 0xc, 0);
    R32(rb + 0xc);
    snooze(2000);
    printf("  CTL=0x%x (should be 0)\n", R32(rb+0xc));

    // Step 2: GPU render domain reset (Ironlake specific)
    printf("\n=== Step 2: ILK GPU Render Reset ===\n");
    W32(ILK_GDSR, ILK_GRDOM_RENDER | ILK_GRDOM_RESET_EN);

    // Wait for reset to complete (bit 31 clears)
    bool resetOk = false;
    for (int i = 0; i < 500; i++) {
        snooze(1000);
        uint32 val = R32(ILK_GDSR);
        if (!(val & ILK_GRDOM_RESET_EN)) {
            printf("  Render reset complete after %d ms\n", i + 1);
            resetOk = true;
            break;
        }
    }
    if (!resetOk) {
        printf("  Render reset timeout, trying full reset...\n");
        W32(ILK_GDSR, 0);
        snooze(5000);
    }

    // Step 3: Clear GDSR
    W32(ILK_GDSR, 0);
    snooze(2000);

    // Step 4: Reinit ring
    printf("\n=== Step 3: Ring Reinit ===\n");
    W32(rb + 0x04, 0);  // HEAD = 0
    R32(rb + 0x04);
    W32(rb + 0x00, 0);  // TAIL = 0
    R32(rb + 0x00);
    snooze(1000);

    // START should still have the correct value from initial driver init
    // But let's make sure
    uint32 startVal = R32(rb + 0x08);
    printf("  START after reset = 0x%08x\n", startVal);

    // If START is 0, it's because the ring is at GTT offset 0 (normal)
    // Re-enable ring
    memset((void*)ring.base, 0, ring.size);
    uint32 ctlVal = ((ring.size - 4096) & 0x001ff000) | 1;
    W32(rb + 0xc, ctlVal);
    R32(rb + 0xc);
    snooze(5000);

    ring.position = 0;
    ring.space_left = ring.size;
    release_lock(&ring.lock);

    printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc));

    // Step 5: Test command execution
    printf("\n=== Step 4: Command Test ===\n");
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
    *(uint32*)(ring.base + pos) = 0xA11CE042; pos += 4;
    ring.position = pos;
    ring.space_left = ring.size - pos;
    atomic_add(&barrier, 1);
    W32(rb + 0x00, pos);
    release_lock(&ring.lock);

    for (int w = 0; w < 20; w++) {
        snooze(5000);
        uint32 head = R32(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
        uint32 val = *scratch;
        if (val == 0xA11CE042) {
            printf("  [OK] GPU ALIVE! HEAD=0x%x scratch=0x%x (%dms)\n",
                head, val, (w+1)*5);
            close(fd);
            return 0;
        }
    }

    printf("  [FAIL] GPU still not responding\n");
    printf("  HEAD=0x%x TAIL=0x%x\n", R32(rb+4), R32(rb+0));
    printf("  You may need to reboot.\n");

    close(fd);
    return 1;
}
