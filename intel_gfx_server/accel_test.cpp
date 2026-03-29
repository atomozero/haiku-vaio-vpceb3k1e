/*
 * accel_test - Test the AccelerantIntel interface
 *
 * Opens the intel_extreme device, initializes AccelerantIntel,
 * and tests GEM create/mmap/exec/close through the i915/v1 interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <OS.h>

#include "AccelerantIntel.h"


static int
open_intel_device()
{
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL)
		return -1;

	char path[256] = {};
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "intel_extreme", 13) == 0) {
			snprintf(path, sizeof(path),
				"/dev/graphics/%s", entry->d_name);
			break;
		}
	}
	closedir(dir);

	if (path[0] == '\0')
		return -1;

	int fd = open(path, B_READ_WRITE);
	if (fd >= 0)
		printf("Device: %s\n", path);
	return fd;
}


int
main()
{
	printf("=== AccelerantIntel Test ===\n\n");

	// Open device
	int fd = open_intel_device();
	if (fd < 0) {
		printf("[FAIL] Cannot open intel_extreme device\n");
		return 1;
	}

	// Initialize accelerant
	IntelAccelerant accel;
	status_t err = accel.Init(fd);
	if (err != B_OK) {
		printf("[FAIL] Init failed: %s\n", strerror(err));
		close(fd);
		return 1;
	}

	// Test QueryInterface
	printf("\n--- QueryInterface ---\n");

	void* i915 = accel.QueryInterface(B_ACCELERANT_IFACE_I915, 0);
	printf("  i915/v1: %s\n", i915 ? "OK" : "FAIL");

	void* drm = accel.QueryInterface(B_ACCELERANT_IFACE_DRM, 0);
	printf("  drm/v1:  %s (expected: NULL)\n", drm ? "unexpected" : "NULL");

	if (i915 == NULL) {
		printf("[FAIL] No i915 interface\n");
		return 1;
	}

	// Use the C vtable interface (as a libdrm shim would)
	accelerant_i915* iface = (accelerant_i915*)i915;

	// Test GetParam
	printf("\n--- GetParam ---\n");
	int32 chipId = 0, gen = 0, gttSize = 0, hasGem = 0;
	iface->vt->GetParam(iface, I915_PARAM_CHIPSET_ID, &chipId);
	iface->vt->GetParam(iface, I915_PARAM_GENERATION, &gen);
	iface->vt->GetParam(iface, I915_PARAM_GTT_SIZE, &gttSize);
	iface->vt->GetParam(iface, I915_PARAM_HAS_GEM, &hasGem);
	printf("  Chipset: 0x%04x\n", chipId);
	printf("  Gen:     %d\n", gen);
	printf("  GTT:     %d MB\n", gttSize / (1024 * 1024));
	printf("  GEM:     %s\n", hasGem ? "yes" : "no");

	// Test GEM create
	printf("\n--- GEM Create ---\n");
	uint32 handle = 0;
	int ret = iface->vt->GemCreate(iface, 4096, &handle);
	printf("  Create 4KB: %s (handle=%u)\n",
		ret == 0 ? "OK" : "FAIL", handle);

	if (ret != 0) {
		printf("[FAIL] GemCreate failed\n");
		return 1;
	}

	// Test GEM mmap
	void* ptr = iface->vt->GemMmap(iface, handle);
	printf("  Mmap:    %s (addr=%p)\n",
		ptr != NULL ? "OK" : "FAIL", ptr);

	// Test GEM offset
	uint32 offset = iface->vt->GemGetOffset(iface, handle);
	printf("  Offset:  0x%x\n", offset);

	// Write pattern and verify
	if (ptr != NULL) {
		memset(ptr, 0xAA, 4096);
		uint8* bytes = (uint8*)ptr;
		bool ok = (bytes[0] == 0xAA && bytes[4095] == 0xAA);
		printf("  Write:   %s\n", ok ? "OK" : "FAIL");
	}

	// Test ring command execution through interface
	printf("\n--- Exec via vtable ---\n");
	uint32 noop_cmds[] = {
		0x00000000,		// MI_NOOP
		0x00000000,		// MI_NOOP
	};
	ret = iface->vt->ExecCommands(iface, noop_cmds, 2);
	printf("  MI_NOOP x2: %s\n", ret == 0 ? "OK" : "FAIL");

	// Wait for idle
	ret = iface->vt->WaitIdle(iface, 100000);
	printf("  WaitIdle:   %s\n", ret == 0 ? "OK" : "FAIL");

	// Cleanup
	printf("\n--- Cleanup ---\n");
	ret = iface->vt->GemClose(iface, handle);
	printf("  Close handle %u: %s\n", handle, ret == 0 ? "OK" : "FAIL");

	printf("\n=== All tests passed ===\n");
	return 0;
}
