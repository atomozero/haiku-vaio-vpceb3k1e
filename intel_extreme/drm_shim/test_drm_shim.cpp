/*
 * test_drm_shim — Verify the DRM shim works: create/map/close BOs.
 */
#include <stdio.h>
#include <string.h>
#include "haiku_drm_intel.h"

int main() {
	printf("=== DRM Shim Test ===\n\n");

	int fd = haiku_drm_open();
	if (fd < 0) { printf("FAIL: cannot open\n"); return 1; }

	/* Test GETPARAM */
	int32_t chip_id = 0;
	struct drm_i915_getparam gp = { I915_PARAM_CHIPSET_ID, &chip_id };
	int r = haiku_drm_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	printf("GETPARAM chipset_id: 0x%04x (%s)\n", chip_id,
		r == 0 ? "OK" : "FAIL");

	/* Test GET_APERTURE */
	struct drm_i915_gem_get_aperture ap = {};
	r = haiku_drm_ioctl(fd, DRM_IOCTL_I915_GEM_GET_APERTURE, &ap);
	printf("GET_APERTURE: %llu MB (%s)\n",
		(unsigned long long)(ap.aper_size / 1024 / 1024),
		r == 0 ? "OK" : "FAIL");

	/* Test GEM_CREATE */
	struct drm_i915_gem_create cr = {};
	cr.size = 65536;
	r = haiku_drm_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &cr);
	printf("GEM_CREATE 64KB: handle=%u (%s)\n", cr.handle,
		r == 0 ? "OK" : "FAIL");

	if (r == 0) {
		/* Test GEM_MMAP */
		struct drm_i915_gem_mmap mm = {};
		mm.handle = cr.handle;
		mm.offset = 0;
		mm.size = 65536;
		r = haiku_drm_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &mm);
		printf("GEM_MMAP: addr=%p (%s)\n", (void*)(uintptr_t)mm.addr_ptr,
			r == 0 ? "OK" : "FAIL");

		if (r == 0) {
			/* Write + read back */
			uint32_t* p = (uint32_t*)(uintptr_t)mm.addr_ptr;
			p[0] = 0xDEADBEEF;
			p[1] = 0x12345678;
			printf("Write/Read: p[0]=0x%08x p[1]=0x%08x (%s)\n",
				p[0], p[1],
				(p[0] == 0xDEADBEEF && p[1] == 0x12345678) ? "OK" : "FAIL");
		}

		/* Test GEM_CLOSE */
		struct drm_gem_close cl = { cr.handle, 0 };
		r = haiku_drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
		printf("GEM_CLOSE: %s\n", r == 0 ? "OK" : "FAIL");
	}

	/* Test multiple BOs */
	printf("\nBulk test: 10 BOs...\n");
	uint32_t handles[10];
	for (int i = 0; i < 10; i++) {
		struct drm_i915_gem_create c = {};
		c.size = 4096;
		r = haiku_drm_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &c);
		handles[i] = c.handle;
	}
	for (int i = 0; i < 10; i++) {
		struct drm_gem_close c = { handles[i], 0 };
		haiku_drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
	}
	printf("10 BOs created and freed: OK\n");

	haiku_drm_close(fd);
	printf("\nAll tests passed!\n");
	return 0;
}
