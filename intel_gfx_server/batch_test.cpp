#include <stdio.h>
#include <string.h>
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

    // Strategy: put the batch buffer at the END of the ring buffer
    // (which we KNOW has valid GTT entries for instruction fetch).
    // Ring is 64KB at GTT 0x0000. Use the last 4KB (0xF000-0xFFFF)
    // as batch space, and scratch (0x10000) as target.
    uint32 batchGTT = ring.offset + ring.size - 0x1000;  // last 4KB of ring
    uint32 targetGTT = 0x10000;  // scratch (proven accessible by MI_STORE)

    volatile uint32* batchCPU = (volatile uint32*)(
        (uint8*)ring.base + ring.size - 0x1000);
    volatile uint32* targetCPU = (volatile uint32*)(
        (uint8*)si->graphics_memory + targetGTT);

    // Clear target
    *targetCPU = 0;

    // Build batch buffer at GTT 0x10000
    int i = 0;
    batchCPU[i++] = 0x10400002;  // MI_STORE_DATA_IMM | GGTT
    batchCPU[i++] = 0;
    batchCPU[i++] = targetGTT;   // write to target
    batchCPU[i++] = 0xBAADF00D;
    batchCPU[i++] = 0x05000000;  // MI_BATCH_BUFFER_END
    batchCPU[i++] = 0;           // padding

    __asm__ __volatile__("sfence" ::: "memory");

    printf("Batch at GTT 0x%x, target at GTT 0x%x\n", batchGTT, targetGTT);
    printf("Target before: 0x%08x\n", *targetCPU);
    printf("HEAD=0x%x TAIL=0x%x\n",
        R32(ring.register_base + 4) & 0x1ffffc,
        R32(ring.register_base + 0));

    // Submit MI_BATCH_BUFFER_START via ring
    acquire_lock(&ring.lock);

    uint32* cmd = (uint32*)(ring.base + ring.position);
    // MI_FLUSH with instruction cache invalidate (before batch fetch)
    cmd[0] = 0x02000002;  // MI_FLUSH | STATE_INSTRUCTION_CACHE_INVALIDATE
    cmd[1] = 0;           // MI_NOOP
    // MI_BATCH_BUFFER_START
    cmd[2] = 0x18800100;  // MI_BATCH_BUFFER_START | NON_SECURE
    cmd[3] = batchGTT;    // GTT offset
    // MI_FLUSH after batch returns
    cmd[4] = 0x02000000;  // MI_FLUSH
    cmd[5] = 0;           // MI_NOOP

    ring.position = (ring.position + 24) & (ring.size - 1);
    ring.space_left -= 24;

    __asm__ __volatile__("sfence" ::: "memory");
    W32(ring.register_base + 0, ring.position);

    release_lock(&ring.lock);

    snooze(10000);
    __asm__ __volatile__("lfence" ::: "memory");

    uint32 targetVal = *targetCPU;
    printf("Target after:  0x%08x (%s)\n", targetVal,
        targetVal == 0xBAADF00D ? "BATCH WORKS!" : "FAIL");
    printf("HEAD=0x%x TAIL=0x%x\n",
        R32(ring.register_base + 4) & 0x1ffffc,
        R32(ring.register_base + 0));

    close(fd);
    return targetVal == 0xBAADF00D ? 0 : 1;
}
