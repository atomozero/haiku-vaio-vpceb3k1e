#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "commands.h"
#include "media_pipeline.h"
#include "gpu_bo.h"

extern accelerant_info* gInfo;

int main() {
    printf("=== Ring Status Check ===\n");
    int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
    if (fd < 0) { printf("no device\n"); return 1; }
    intel_get_private_data data;
    data.magic = INTEL_PRIVATE_DATA_MAGIC;
    ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data));
    gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
    memset(gInfo, 0, sizeof(accelerant_info));
    gInfo->is_clone = true; gInfo->device = fd;
    gInfo->shared_info_area = clone_area("chk",
        (void**)&gInfo->shared_info, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
    gInfo->regs_area = clone_area("chk_r",
        (void**)&gInfo->registers, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA,
        gInfo->shared_info->registers_area);

    ring_buffer& ring = gInfo->shared_info->primary_ring_buffer;
    volatile uint32* regs = (volatile uint32*)gInfo->registers;
    uint32 rb = ring.register_base;

    uint32 head = regs[(rb+4)/4] & 0x001FFFFC;
    uint32 tail = regs[rb/4];
    uint32 ctl = regs[(rb+0xC)/4];

    printf("Ring: HEAD=0x%x TAIL=0x%x CTL=0x%x sw_pos=%u\n",
        head, tail, ctl, ring.position);
    printf("HEAD==TAIL: %s (ring %s)\n",
        head == tail ? "YES" : "NO",
        head == tail ? "idle/caught up" : "commands pending");

    // Write a simple MI_NOOP+MI_STORE_DATA_IMM and check if HEAD advances
    media_pipeline_context ctx;
    status_t st = media_pipeline_init(&ctx);
    if (st != B_OK) { printf("media init failed\n"); return 1; }
    
    volatile uint32* mk = (volatile uint32*)ctx.marker_bo.cpu_addr;
    *mk = 0;
    asm volatile("mfence" ::: "memory");

    printf("\nSubmitting MI_STORE_DATA_IMM via QueueCommands...\n");
    {
        QueueCommands queue(ring);
        queue.MakeSpace(4);
        queue.Write((0x20 << 23) | (1 << 22) | 2);
        queue.Write(0);
        queue.Write(ctx.marker_bo.gtt_offset);
        queue.Write(0xBEEFCAFE);
    }

    uint32 tail2 = regs[rb/4];
    printf("TAIL after submit: 0x%x\n", tail2);

    // Busy-wait up to 100ms
    bigtime_t t0 = system_time();
    while (*mk != 0xBEEFCAFE && (system_time() - t0) < 100000)
        ;
    
    uint32 head2 = regs[(rb+4)/4] & 0x001FFFFC;
    printf("HEAD after wait: 0x%x (was 0x%x)\n", head2, head);
    printf("Marker: 0x%08x %s\n", *mk,
        *mk == 0xBEEFCAFE ? ">>> RING IS ALIVE! <<<" : "RING DEAD");
    
    if (*mk == 0xBEEFCAFE) {
        printf("\nRing works! HEAD advanced %d bytes.\n",
            (int)(head2 - head));
    }

    media_pipeline_uninit(&ctx);
    close(fd);
    return 0;
}
