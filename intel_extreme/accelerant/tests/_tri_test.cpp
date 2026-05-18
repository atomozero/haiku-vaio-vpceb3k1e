#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include "intel_extreme.h"
#include "lock.h"
#include "accelerant.h"
#include "render.h"

extern accelerant_info* gInfo;

static bool init_gpu(void) {
    int fd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
    if (fd < 0) return false;
    intel_get_private_data data;
    data.magic = INTEL_PRIVATE_DATA_MAGIC;
    if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
        close(fd); return false;
    }
    gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
    memset(gInfo, 0, sizeof(accelerant_info));
    gInfo->is_clone = true;
    gInfo->device = fd;
    gInfo->shared_info_area = clone_area("tri",
        (void**)&gInfo->shared_info, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
    if (gInfo->shared_info_area < B_OK) { free(gInfo); close(fd); return false; }
    gInfo->regs_area = clone_area("tri_r",
        (void**)&gInfo->registers, B_ANY_ADDRESS,
        B_READ_AREA | B_WRITE_AREA,
        gInfo->shared_info->registers_area);
    if (gInfo->regs_area < B_OK) {
        delete_area(gInfo->shared_info_area); free(gInfo); close(fd); return false;
    }
    return true;
}

int main() {
    printf("=== 3D TRILIST Test ===\n");
    if (!init_gpu()) { printf("GPU init failed\n"); return 1; }

    uint32 w = gInfo->shared_info->current_mode.timing.h_display;
    uint32 h = gInfo->shared_info->current_mode.timing.v_display;
    printf("Display %ux%u\n", w, h);

    status_t st = render_init_clone();
    printf("render_init_clone: %s\n", st == B_OK ? "OK" : strerror(st));
    if (st != B_OK) return 1;

    float cx = w/2.0f, cy = h/2.0f;

    printf("\nDraw 1 (alternates RECT/TRI):\n");
    st = render_draw_triangle(0xFFFF0000, cx, cy-200, cx-174, cy+100, cx+174, cy+100);
    printf("  status: %s\n", st == B_OK ? "OK" : strerror(st));

    printf("\nDraw 2:\n");
    st = render_draw_triangle(0xFF00FF00, cx-100, cy-150, cx+100, cy-150, cx, cy+150);
    printf("  status: %s\n", st == B_OK ? "OK" : strerror(st));

    printf("\nCheck screen + syslog. Press Enter.\n");
    getchar();

    render_uninit();
    delete_area(gInfo->regs_area);
    delete_area(gInfo->shared_info_area);
    close(gInfo->device); free(gInfo);
    return 0;
}
