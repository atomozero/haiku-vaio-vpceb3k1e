/*
 * GEM-like buffer object manager for Intel Gen5 on Haiku.
 * Allocates GPU memory from the GTT aperture, tracks buffer
 * objects with handles, and supports batch buffer submission.
 */
#ifndef GEM_MANAGER_H
#define GEM_MANAGER_H

#include <OS.h>
#include <SupportDefs.h>
#include "intel_extreme.h"

// Maximum buffer objects per client
#define GEM_MAX_OBJECTS		256

// Scratch memory region (for fencing/sync)
#define GEM_SCRATCH_GTT		0x10000
#define GEM_SCRATCH_SIZE	0x1000		// 4KB scratch page

// Allocatable region: after ring (0x10000) and scratch (0x11000),
// before framebuffer (0x21000).  Additional space after framebuffer.
#define GEM_ALLOC_START		0x11000
#define GEM_ALLOC_FB		0x21000		// framebuffer starts here


struct gem_buffer {
	uint32		handle;			// client-visible handle (1-based)
	uint32		gtt_offset;		// GTT byte offset
	uint32		size;			// size in bytes (page-aligned)
	addr_t		cpu_addr;		// CPU virtual address (via GTT aperture)
	bool		in_use;			// handle is allocated
};


class GemManager {
public:
					GemManager();
					~GemManager();

	status_t		Init(intel_shared_info* sharedInfo, volatile uint8* regs,
						int deviceFd);

	// Buffer object operations (GEM-like)
	status_t		CreateBuffer(uint32 size, uint32* outHandle);
	void*			MapBuffer(uint32 handle);
	uint32			GetOffset(uint32 handle);
	status_t		CloseBuffer(uint32 handle);

	// Batch buffer execution
	status_t		ExecBatch(uint32 batchHandle, uint32 batchLen);

	// Sync
	status_t		WaitIdle(bigtime_t timeout = 1000000);
	uint32			GetLastSeq() const { return fLastSeq; }

private:
	void			_RingWrite(uint32 value);
	void			_RingFlush();

	inline uint32	_ReadReg(uint32 off)
					{ return *(volatile uint32*)(fRegs + off); }
	inline void		_WriteReg(uint32 off, uint32 val)
					{ *(volatile uint32*)(fRegs + off) = val; }

	intel_shared_info*	fSharedInfo;
	volatile uint8*		fRegs;
	ring_buffer*		fRing;

	gem_buffer		fObjects[GEM_MAX_OBJECTS];
	uint32			fNextHandle;

	int				fDeviceFd;		// device fd for kernel ioctls

	// Fencing
	uint32			fLastSeq;
	volatile uint32* fScratch;		// scratch page for fence writes
};


#endif	// GEM_MANAGER_H
