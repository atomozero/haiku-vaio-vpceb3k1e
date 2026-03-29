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

    // Use scratch page (GTT 0x10000) as batch buffer location
    // We know the GPU can access this (MI_STORE_DATA_IMM proved it)
    uint32 batchGTT = 0x10000;
    uint32 targetGTT = 0x10800;  // target within same page (offset 0x800)
    
    volatile uint32* batchCPU = (volatile uint32*)(
        (uint8*)si->graphics_memory + batchGTT);
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
    cmd[0] = 0x18800000 | (1 << 8);  // MI_BATCH_BUFFER_START | NON_SECURE
    cmd[1] = batchGTT;    // GTT offset
    cmd[2] = 0x02000000;  // MI_FLUSH
    cmd[3] = 0;            // MI_NOOP

    ring.position = (ring.position + 16) & (ring.size - 1);
    ring.space_left -= 16;

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
