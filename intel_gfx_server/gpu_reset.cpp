#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
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

    printf("=== GPU State Before Reset ===\n");
    printf("  RING: HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc), R32(rb+8));
    printf("  INSTDONE=0x%08x\n", R32(0x206c));
    printf("  ERROR=0x%08x\n", R32(0x20b0));
    printf("  PGTBL_ER=0x%08x\n", R32(0x2024));
    printf("  IPEIR=0x%08x\n", R32(0x2064));  // Instruction parser error
    printf("  IPEHR=0x%08x\n", R32(0x2068));  // Error header
    printf("  EIR=0x%08x\n", R32(0x20b0));    // Error identity

    // Clear errors first
    W32(0x2024, R32(0x2024));  // Clear PGTBL_ER (W1C)
    W32(0x20b0, R32(0x20b0));  // Clear ERROR (W1C)

    printf("\n=== Full Ring Reset ===\n");
    acquire_lock(&ring.lock);

    // 1. Stop ring
    W32(rb + 0xc, 0);
    R32(rb + 0xc);
    snooze(2000);

    // 2. Force HEAD to 0
    W32(rb + 0x4, 0);
    R32(rb + 0x4);
    snooze(2000);

    // 3. Force TAIL to 0
    W32(rb + 0x0, 0);
    R32(rb + 0x0);
    snooze(1000);

    // 4. Re-write START (just in case)
    uint32 startAddr = R32(rb + 0x8);
    printf("  Ring START = 0x%08x\n", startAddr);
    W32(rb + 0x8, startAddr);
    R32(rb + 0x8);
    snooze(1000);

    // 5. Clear ring memory (fill with MI_NOOP)
    memset((void*)ring.base, 0, ring.size);

    // 6. Re-enable ring
    W32(rb + 0xc,
        ((ring.size - 4096) & 0x001ff000) | 1);
    R32(rb + 0xc);
    snooze(5000);

    // 7. Reset software state
    ring.position = 0;
    ring.space_left = ring.size;

    release_lock(&ring.lock);

    printf("  After: HEAD=0x%x TAIL=0x%x CTL=0x%x\n",
        R32(rb+4), R32(rb+0), R32(rb+0xc));
    printf("  INSTDONE=0x%08x\n", R32(0x206c));
    printf("  ERROR=0x%08x\n", R32(0x20b0));

    // Test with MI_NOOP padding then MI_STORE_DATA_IMM
    volatile uint32* scratch = (volatile uint32*)(
        (uint8*)si->graphics_memory + 0x10000);
    *scratch = 0;
    int32 barrier = 0;
    atomic_add(&barrier, 1);

    acquire_lock(&ring.lock);
    uint32 pos = 0;
    // 8 MI_NOOPs as warmup
    for (int i = 0; i < 8; i++) {
        *(uint32*)(ring.base + pos) = 0; pos += 4;
    }
    // MI_STORE_DATA_IMM
    *(uint32*)(ring.base + pos) = 0x10400002; pos += 4;
    *(uint32*)(ring.base + pos) = 0;          pos += 4;
    *(uint32*)(ring.base + pos) = 0x10000;    pos += 4;
    *(uint32*)(ring.base + pos) = 0xCAFEBABE; pos += 4;
    // 4 more MI_NOOPs
    for (int i = 0; i < 4; i++) {
        *(uint32*)(ring.base + pos) = 0; pos += 4;
    }

    ring.position = pos;
    ring.space_left = ring.size - pos;
    atomic_add(&barrier, 1);
    W32(rb + 0x0, pos);  // TAIL
    release_lock(&ring.lock);

    printf("\n=== Command Test ===\n");
    printf("  Submitted %d DWORDs, TAIL=0x%x\n", pos/4, pos);

    for (int wait = 0; wait < 20; wait++) {
        snooze(5000);
        uint32 head = R32(rb + 4) & INTEL_RING_BUFFER_HEAD_MASK;
        uint32 val = *scratch;
        if (val == 0xCAFEBABE) {
            printf("  [OK] HEAD=0x%x scratch=0x%08x (after %d ms)\n",
                head, val, (wait+1)*5);
            break;
        }
        if (wait == 19) {
            printf("  [FAIL] HEAD=0x%x scratch=0x%08x (timeout)\n",
                head, val);
        }
    }

    close(fd);
    return 0;
}
