/*
 * AccelerantAddon - shared library entry point for accelerant2
 *
 * Exports instantiate_accelerant() which creates an IntelAccelerant
 * wrapped in an Accelerant-compatible object. Loadable by
 * AccelerantRoster via dlopen/dlsym.
 *
 * Build: g++ -shared -o intel_gfx.accelerant2 AccelerantAddon.cpp
 *        AccelerantIntel.cpp GemManager.cpp ...
 * Install: copy to /boot/system/non-packaged/add-ons/accelerants/
 */

#include <stdio.h>
#include <string.h>
#include <new>

#include <Referenceable.h>

#include "AccelerantIntel.h"


// ---------------------------------------------------------------
// Minimal AccelerantBase/Accelerant compatibility
//
// When X547's accelerant2 library becomes available in Haiku,
// replace this with #include "AccelerantRoster.h" and link
// against libaccelerant2.so.
// ---------------------------------------------------------------

#ifndef B_ACCELERANT_IFACE_BASE
#define B_ACCELERANT_IFACE_BASE "base/v1"
#endif

class AccelerantBase {
public:
	virtual int32		AcquireReference() = 0;
	virtual int32		ReleaseReference() = 0;
	virtual void*		QueryInterface(const char* iface,
							uint32 version) = 0;
};


// ---------------------------------------------------------------
// IntelAccelerantAddon - bridges Accelerant base with our impl
// ---------------------------------------------------------------

class IntelAccelerantAddon : public BReferenceable,
	public AccelerantBase {
public:
						IntelAccelerantAddon();
	virtual				~IntelAccelerantAddon();

	status_t			Init(int fd);

	// AccelerantBase
	int32				AcquireReference() override;
	int32				ReleaseReference() override;
	void*				QueryInterface(const char* iface,
							uint32 version) override;

private:
	IntelAccelerant		fImpl;
};


IntelAccelerantAddon::IntelAccelerantAddon()
{
}


IntelAccelerantAddon::~IntelAccelerantAddon()
{
	printf("IntelAccelerantAddon: destroyed\n");
}


status_t
IntelAccelerantAddon::Init(int fd)
{
	return fImpl.Init(fd);
}


int32
IntelAccelerantAddon::AcquireReference()
{
	return BReferenceable::AcquireReference();
}


int32
IntelAccelerantAddon::ReleaseReference()
{
	return BReferenceable::ReleaseReference();
}


void*
IntelAccelerantAddon::QueryInterface(const char* iface, uint32 version)
{
	if (strcmp(iface, B_ACCELERANT_IFACE_BASE) == 0 && version <= 0)
		return static_cast<AccelerantBase*>(this);

	// Delegate Intel-specific interfaces
	return fImpl.QueryInterface(iface, version);
}


// ---------------------------------------------------------------
// Entry point - called by AccelerantRoster after dlopen
// ---------------------------------------------------------------

// Keep a global pointer so C helpers can reach the object
static IntelAccelerantAddon* sInstance = NULL;


extern "C" status_t _EXPORT
instantiate_accelerant(void** outAcc, int fd)
{
	printf("instantiate_accelerant: fd=%d\n", fd);

	IntelAccelerantAddon* addon = new(std::nothrow) IntelAccelerantAddon();
	if (addon == NULL)
		return B_NO_MEMORY;

	status_t err = addon->Init(fd);
	if (err != B_OK) {
		delete addon;
		return err;
	}

	sInstance = addon;
	*outAcc = addon;
	return B_OK;
}


// C helper for QueryInterface - avoids C++ vtable ABI issues
// when called from a separately compiled binary via dlsym.
extern "C" void* _EXPORT
accelerant_query_interface(void* acc, const char* iface, uint32 version)
{
	if (sInstance == NULL)
		return NULL;
	return sInstance->QueryInterface(iface, version);
}
