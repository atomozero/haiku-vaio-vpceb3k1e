/*
 * irq_health — detects the MSI edge-loss signature in the interrupt
 * registers of the intel_extreme GPU.
 *
 * MSI is edge-triggered: if an IIR bit stays pending while enabled and
 * unmasked, the interrupt condition never deasserts, no new message is
 * ever generated and ALL delivery (vblank included) is dead until
 * reboot. This tool samples DEIIR/GTIIR twice; the same enabled bit
 * pending in both samples while the system is idle is the signature.
 *
 * Reads go through the cloned register area (MMIO reads work from
 * userspace; writes do not — see CLAUDE.md).
 *
 * Build: g++ -Wall -O2 -o irq_health irq_health.cpp \
 *        -I../intel_extreme -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *        -I/boot/system/develop/headers/private/graphics/common -lbe
 * Exit code: 0 = healthy, 1 = stuck pending bits (edge lost), 2 = setup error.
 */

#include <OS.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <intel_extreme.h>

static const uint32 kDEIMR = 0x44004;
static const uint32 kDEIIR = 0x44008;
static const uint32 kDEIER = 0x4400c;
static const uint32 kGTIMR = 0x44014;
static const uint32 kGTIIR = 0x44018;
static const uint32 kGTIER = 0x4401c;


int
main()
{
	int fd = open("/dev/graphics/intel_extreme_000200", O_RDWR);
	if (fd < 0) {
		printf("irq_health: cannot open GPU device\n");
		return 2;
	}

	intel_get_private_data privateData;
	privateData.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, INTEL_GET_PRIVATE_DATA, &privateData,
			sizeof(privateData)) != 0) {
		printf("irq_health: INTEL_GET_PRIVATE_DATA failed\n");
		return 2;
	}

	intel_shared_info* shared;
	area_id sharedArea = clone_area("irq_health shared", (void**)&shared,
		B_ANY_ADDRESS, B_READ_AREA, privateData.shared_info_area);
	if (sharedArea < B_OK)
		return 2;

	uint8* regs;
	area_id regsArea = clone_area("irq_health regs", (void**)&regs,
		B_ANY_ADDRESS, B_READ_AREA, shared->registers_area);
	if (regsArea < B_OK)
		return 2;

	#define REG(offset) (*(volatile uint32*)(regs + (offset)))

	uint32 dePending1 = REG(kDEIIR) & REG(kDEIER) & ~REG(kDEIMR);
	uint32 gtPending1 = REG(kGTIIR) & REG(kGTIER) & ~REG(kGTIMR);
	snooze(300000);
	uint32 dePending2 = REG(kDEIIR) & REG(kDEIER) & ~REG(kDEIMR);
	uint32 gtPending2 = REG(kGTIIR) & REG(kGTIER) & ~REG(kGTIMR);

	uint32 deStuck = dePending1 & dePending2;
	uint32 gtStuck = gtPending1 & gtPending2;

	printf("DEIER 0x%08" B_PRIx32 "  DEIMR 0x%08" B_PRIx32
		"  DEIIR 0x%08" B_PRIx32 "\n", REG(kDEIER), REG(kDEIMR),
		REG(kDEIIR));
	printf("GTIER 0x%08" B_PRIx32 "  GTIMR 0x%08" B_PRIx32
		"  GTIIR 0x%08" B_PRIx32 "\n", REG(kGTIER), REG(kGTIMR),
		REG(kGTIIR));

	if (deStuck != 0 || gtStuck != 0) {
		printf("irq_health: STUCK pending bits (DE 0x%08" B_PRIx32
			", GT 0x%08" B_PRIx32 ") — MSI edge lost, interrupt "
			"delivery is dead\n", deStuck, gtStuck);
		return 1;
	}

	printf("irq_health: OK — no stuck enabled+pending interrupt bits\n");
	return 0;
}
