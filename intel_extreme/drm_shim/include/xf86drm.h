/*
 * xf86drm.h — Haiku shim for Mesa's libdrm dependency.
 *
 * Redirects drmIoctl() to haiku_drm_ioctl() after extracting
 * the command number from the Linux _IOC-encoded request.
 */
#ifndef XF86DRM_H
#define XF86DRM_H

#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

/* Linux _IOC encoding compatibility (Haiku doesn't have these) */
#ifndef _IOC_NRBITS
#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IOC(dir,type,nr,size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_NR(nr)    (((nr) >> _IOC_NRSHIFT) & ((1 << _IOC_NRBITS) - 1))
#define _IO(type,nr)        _IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)  _IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW(type,nr,size)  _IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#define _IOWR(type,nr,size) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))
#endif

/* All DRM functions are exported from libdrm_shim.so */
#ifdef __cplusplus
extern "C" {
#endif
int haiku_drm_open(void);
void haiku_drm_close(int fd);
int haiku_drm_ioctl(int fd, unsigned long request, void* arg);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmOpen(const char *name, const char *busid);
int drmClose(int fd);
#ifdef __cplusplus
}
#endif

/* Version info — crocus checks for "i915" */
typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len; char *name;
	int date_len; char *date;
	int desc_len; char *desc;
} drmVersion, *drmVersionPtr;

static inline drmVersionPtr drmGetVersion(int fd)
{
	(void)fd;
	static char n[] = "i915", d[] = "20260511", ds[] = "Haiku DRM shim";
	static drmVersion v = { 1,6,0, 4,n, 8,d, 14,ds };
	return &v;
}
static inline void drmFreeVersion(drmVersionPtr v) { (void)v; }

/* --- Syncobj stubs (all return -1 = not supported) --- */
static inline int drmSyncobjCreate(int fd, uint32_t f, uint32_t *h)
{ (void)fd;(void)f;(void)h; errno=ENOSYS; return -1; }
static inline int drmSyncobjDestroy(int fd, uint32_t h)
{ (void)fd;(void)h; return -1; }
static inline int drmSyncobjSignal(int fd, const uint32_t *h, uint32_t c)
{ (void)fd;(void)h;(void)c; return -1; }
static inline int drmSyncobjWait(int fd, uint32_t *h, unsigned c, int64_t t, unsigned f, uint32_t *fi)
{ (void)fd;(void)h;(void)c;(void)t;(void)f;(void)fi; return -1; }
static inline int drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t c)
{ (void)fd;(void)h;(void)p;(void)c; return -1; }
static inline int drmSyncobjTimelineWait(int fd, uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *fi)
{ (void)fd;(void)h;(void)p;(void)n;(void)t;(void)f;(void)fi; return -1; }
static inline int drmSyncobjQuery2(int fd, uint32_t *h, uint64_t *p, uint32_t c, uint32_t f)
{ (void)fd;(void)h;(void)p;(void)c;(void)f; return -1; }
static inline int drmSyncobjTransfer(int fd, uint32_t d, uint64_t dp, uint32_t s, uint64_t sp, uint32_t f)
{ (void)fd;(void)d;(void)dp;(void)s;(void)sp;(void)f; return -1; }
static inline int drmSyncobjImportSyncFile(int fd, uint32_t h, int sf)
{ (void)fd;(void)h;(void)sf; return -1; }
static inline int drmSyncobjExportSyncFile(int fd, uint32_t h, int *sf)
{ (void)fd;(void)h;(void)sf; return -1; }
static inline int drmSyncobjReset(int fd, const uint32_t *h, uint32_t c)
{ (void)fd;(void)h;(void)c; return -1; }
static inline int drmSyncobjFDToHandle(int fd, int of, uint32_t *h)
{ (void)fd;(void)of;(void)h; return -1; }
static inline int drmSyncobjHandleToFD(int fd, uint32_t h, int *of)
{ (void)fd;(void)h;(void)of; return -1; }

/* --- Capability / misc stubs --- */
static inline int drmGetCap(int fd, uint64_t cap, uint64_t *val)
{ (void)fd;(void)cap; if(val)*val=0; return -1; }
static inline int drmSetClientCap(int fd, uint64_t c, uint64_t v)
{ (void)fd;(void)c;(void)v; return -1; }
static inline int drmGetNodeTypeFromFd(int fd)
{ (void)fd; return 0; }
static inline char* drmGetDeviceNameFromFd2(int fd)
{ (void)fd; return (char*)"intel_extreme"; }
static inline void drmFree(void *p) { (void)p; }
static inline int drmCommandWriteRead(int fd, unsigned long cmd, void *d, unsigned long sz)
{ (void)fd;(void)cmd;(void)d;(void)sz; return -1; }

#define DRM_CAP_SYNCOBJ_TIMELINE 0x14
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER  2
#define DRM_BUS_PCI      0
#define DRM_BUS_USB      1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X   3
#define DRM_DEVICE_GET_PCI_REVISION 0
#define DRM_DIR_NAME "/dev/graphics"
#define DRM_DEV_NAME "/dev/graphics/intel_extreme"
#define DRM_NAME "drm"

/* --- drmDevice structs for Mesa loader --- */
typedef struct { uint16_t domain; uint8_t bus,dev,func; } drmPciBusInfo;
typedef struct { uint16_t vendor_id,device_id,subvendor_id,subdevice_id; uint8_t revision_id; } drmPciDeviceInfo;
typedef struct { char *fullname; } drmPlatformBusInfo;
typedef struct { char *fullname; } drmHost1xBusInfo;

typedef struct _drmDevice {
	char **nodes; int available_nodes; int bustype;
	union { drmPciBusInfo *pci; drmPlatformBusInfo *platform; drmHost1xBusInfo *host1x; } businfo;
	union { drmPciDeviceInfo *pci; } deviceinfo;
} drmDevice, *drmDevicePtr;

static inline int drmGetDevices2(uint32_t f, drmDevicePtr *d, int m)
{ (void)f;(void)d;(void)m; return 0; }

static inline int drmGetDevice2(int fd, uint32_t f, drmDevicePtr *dev)
{
	(void)fd;(void)f;
	static char n0[] = "/dev/graphics/intel_extreme_000200";
	static char *nodes[] = { n0, NULL, n0 };
	static drmPciBusInfo bi = { 0,0,2,0 };
	static drmPciDeviceInfo di = { 0x8086,0x0046,0,0,2 };
	static drmDevice d = { nodes, (1<<DRM_NODE_PRIMARY)|(1<<DRM_NODE_RENDER), DRM_BUS_PCI, {&bi}, {&di} };
	*dev = &d; return 0;
}
static inline int drmGetDeviceFromDevId(unsigned long long id, uint32_t f, drmDevicePtr *d)
{ (void)id;(void)f;(void)d; return -1; }
static inline void drmFreeDevice(drmDevicePtr *d) { (void)d; }
static inline void drmFreeDevices(drmDevicePtr *d, int c) { (void)d;(void)c; }

static inline int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{ return a == b ? 1 : 0; }

static inline char* drmGetPrimaryDeviceNameFromFd(int fd)
{ (void)fd; return (char*)"/dev/graphics/intel_extreme_000200"; }

static inline char* drmGetRenderDeviceNameFromFd(int fd)
{ (void)fd; return (char*)"/dev/graphics/intel_extreme_000200"; }

static inline int drmGetMagic(int fd, uint32_t *magic)
{ (void)fd; if(magic) *magic=0; return 0; }

static inline int drmAuthMagic(int fd, uint32_t magic)
{ (void)fd; (void)magic; return 0; }

/* --- DRM Prime (GEM handle <-> fd) --- */
static inline int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{ (void)fd;(void)handle;(void)flags;(void)prime_fd; errno=ENOSYS; return -1; }
static inline int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{ (void)fd;(void)prime_fd;(void)handle; errno=ENOSYS; return -1; }

/* --- DRM Mode (KMS) stubs --- */
#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC O_CLOEXEC
#endif
#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

/* Mode structs/ioctls — include Mesa's drm_mode.h if available */
#if __has_include("drm-uapi/drm_mode.h")
#include "drm-uapi/drm_mode.h"
#else
struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags, handle, pitch;
	uint64_t size;
};
struct drm_mode_destroy_dumb { uint32_t handle; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
#endif
#ifndef DRM_IOCTL_MODE_CREATE_DUMB
#define DRM_IOCTL_MODE_CREATE_DUMB  _IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB _IOWR('d', 0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#endif

static inline int drmModeAddFB(int fd, uint32_t w, uint32_t h, uint8_t d,
	uint8_t b, uint32_t p, uint32_t bo, uint32_t *buf)
{ (void)fd;(void)w;(void)h;(void)d;(void)b;(void)p;(void)bo;(void)buf; return -1; }
static inline int drmModeRmFB(int fd, uint32_t buf)
{ (void)fd;(void)buf; return -1; }

#include <fcntl.h>

#endif /* XF86DRM_H */
