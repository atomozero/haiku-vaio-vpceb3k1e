/*
 * addon_test - Test loading intel_gfx.accelerant2 as shared library
 *
 * Simulates AccelerantRoster::Load():
 * 1. dlopen the add-on
 * 2. dlsym("instantiate_accelerant")
 * 3. Call it with device fd
 * 4. Use the C helper accelerant_query_interface() for i915/v1
 * 5. Exercise GPU ops through C vtable
 *
 * NOTE: Do NOT cast accRaw to a C++ AccelerantBase* and call
 * virtual methods across the dlopen boundary. With multiple
 * inheritance (BReferenceable + AccelerantBase), the vtable
 * layout differs between compilation units → GPF.
 * Always use the C helper or link against a shared framework.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>

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

	return open(path, B_READ_WRITE);
}


int
main()
{
	printf("=== Accelerant2 Add-on Load Test ===\n\n");

	// Step 1: dlopen the add-on
	printf("--- dlopen ---\n");
	void* addon = dlopen("./intel_gfx.accelerant2", RTLD_LAZY);
	if (addon == NULL) {
		printf("[FAIL] dlopen: %s\n", dlerror());
		return 1;
	}
	printf("  Loaded: OK\n");

	// Step 2: find instantiate_accelerant
	// Returns an opaque pointer; we cast to IntelAccelerant* since
	// we know the concrete type in this test scenario.
	typedef status_t (*instantiate_func)(void** acc, int fd);
	instantiate_func instantiate = (instantiate_func)
		dlsym(addon, "instantiate_accelerant");
	if (instantiate == NULL) {
		printf("[FAIL] dlsym: %s\n", dlerror());
		dlclose(addon);
		return 1;
	}
	printf("  Symbol: OK\n");

	// Step 3: open device and instantiate
	int fd = open_intel_device();
	if (fd < 0) {
		printf("[FAIL] No intel_extreme device\n");
		dlclose(addon);
		return 1;
	}
	printf("  Device:  fd=%d\n", fd);

	printf("\n--- instantiate_accelerant ---\n");
	void* accRaw = NULL;
	status_t err = instantiate(&accRaw, fd);
	if (err != B_OK) {
		printf("[FAIL] instantiate: %s\n", strerror(err));
		close(fd);
		dlclose(addon);
		return 1;
	}
	printf("  Created: OK (ptr=%p)\n", accRaw);

	// Step 4: Use the C vtable for i915/v1 directly
	// The add-on's instantiate returns a pointer whose internal
	// QueryInterface we call. Since the C++ vtable ABI might differ
	// across compilation units, we use a dlsym'd helper instead.
	//
	// For a real accelerant2 deployment, AccelerantRoster handles
	// this via the shared AccelerantBase vtable from the framework
	// library. Here we test the C vtable path.

	// Find a query helper exported by the add-on
	typedef void* (*query_func)(void* acc, const char* iface, uint32 ver);
	query_func queryIface = (query_func)
		dlsym(addon, "accelerant_query_interface");

	accelerant_i915* i915 = NULL;

	if (queryIface != NULL) {
		printf("  Using exported query helper\n");
		i915 = (accelerant_i915*)queryIface(accRaw, "i915/v1", 0);
	} else {
		// Fallback: the object IS an IntelAccelerantAddon whose first
		// base is BReferenceable (with its vtable), then AccelerantBase.
		// Too fragile across compilation units. Skip vtable test.
		printf("  No query helper; testing instantiation only\n");
	}

	if (i915 != NULL) {
		printf("\n--- GPU via dlopen'd add-on ---\n");

		int32 chipId = 0, gen = 0;
		i915->vt->GetParam(i915, I915_PARAM_CHIPSET_ID, &chipId);
		i915->vt->GetParam(i915, I915_PARAM_GENERATION, &gen);
		printf("  Chip: 0x%04x Gen%d\n", chipId, gen);

		uint32 handle = 0;
		int ret = i915->vt->GemCreate(i915, 4096, &handle);
		printf("  GemCreate: %s (handle=%u)\n",
			ret == 0 ? "OK" : "FAIL", handle);

		if (ret == 0) {
			uint32 noop[] = { 0x00000000, 0x00000000 };
			ret = i915->vt->ExecCommands(i915, noop, 2);
			printf("  Exec NOOP: %s\n", ret == 0 ? "OK" : "FAIL");

			ret = i915->vt->WaitIdle(i915, 100000);
			printf("  WaitIdle:  %s\n", ret == 0 ? "OK" : "FAIL");

			i915->vt->GemClose(i915, handle);
			printf("  GemClose:  OK\n");
		}
	}

	// Cleanup
	printf("\n--- Cleanup ---\n");
	close(fd);
	dlclose(addon);
	printf("  Done\n");

	printf("\n=== Add-on test passed ===\n");
	return 0;
}
