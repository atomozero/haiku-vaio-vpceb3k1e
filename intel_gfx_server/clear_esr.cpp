#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <OS.h>
#include "accelerant.h"
#include "intel_extreme.h"

int main() {
	DIR* d = opendir("/dev/graphics");
	char p[256]={};struct dirent* e;
	while((e=readdir(d)))if(strncmp(e->d_name,"intel_extreme",13)==0){snprintf(p,sizeof(p),"/dev/graphics/%s",e->d_name);break;}
	closedir(d);
	int fd=open(p,B_READ_WRITE);
	intel_get_private_data data;data.magic=INTEL_PRIVATE_DATA_MAGIC;
	ioctl(fd,INTEL_GET_PRIVATE_DATA,&data,sizeof(data));
	intel_shared_info* si;volatile uint8* regs;
	clone_area("s",(void**)&si,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,data.shared_info_area);
	clone_area("r",(void**)&regs,B_ANY_ADDRESS,B_READ_AREA|B_WRITE_AREA,si->registers_area);

	auto rr = [&](uint32 o) -> uint32 { return *(volatile uint32*)(regs+o); };
	auto wr = [&](uint32 o, uint32 v) { *(volatile uint32*)(regs+o) = v; };

	printf("Before:\n");
	printf("  ESR=0x%x EIR=0x%x EMR=0x%x\n", rr(0x20b8), rr(0x20b0), rr(0x20b4));
	printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n", rr(0x2034), rr(0x2030), rr(0x203c));

	// Disable ring
	wr(0x203c, 0);
	snooze(5000);

	// Mask all errors
	wr(0x20b4, 0xFFFFFFFF);  // EMR = mask all
	snooze(1000);

	// Clear ESR/EIR by writing back
	wr(0x20b8, rr(0x20b8));
	wr(0x20b0, rr(0x20b0));
	snooze(1000);

	// Reset ring
	wr(0x2030, 0);  // TAIL=0
	snooze(1000);

	// Re-enable ring
	wr(0x203c, 0xF001);
	snooze(5000);

	// Update SW state
	si->primary_ring_buffer.position = 0;
	si->primary_ring_buffer.space_left = si->primary_ring_buffer.size - 16;

	printf("\nAfter:\n");
	printf("  ESR=0x%x EIR=0x%x EMR=0x%x\n", rr(0x20b8), rr(0x20b0), rr(0x20b4));
	printf("  HEAD=0x%x TAIL=0x%x CTL=0x%x\n", rr(0x2034), rr(0x2030), rr(0x203c));

	// Test MI_NOOP
	uint32* rm = (uint32*)si->primary_ring_buffer.base;
	rm[0] = 0x00000000;
	rm[1] = 0x00000000;
	wr(0x2030, 8);  // TAIL=8
	snooze(50000);

	printf("\nTest:\n");
	printf("  HEAD=0x%x (expect 0x8)\n", rr(0x2034) & 0x1FFFF8);
	printf("  %s\n", (rr(0x2034) & 0x1FFFF8) >= 8 ? "GPU ALIVE!" : "Still hung");
	return 0;
}
