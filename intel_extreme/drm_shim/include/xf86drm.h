/*
 * xf86drm.h — Haiku shim for Mesa's libdrm dependency.
 *
 * Redirects drmIoctl() and friends to haiku_drm_ioctl().
 * Mesa crocus includes this via -I path override.
 */
#ifndef XF86DRM_H
#define XF86DRM_H

#include <stdint.h>
#include <errno.h>

#include "haiku_drm_intel.h"

/* drmIoctl — Mesa's main DRM entry point.
 * On Linux this is ioctl(fd, DRM_IOCTL_BASE + request, arg).
 * We redirect to our shim. */
static inline int
drmIoctl(int fd, unsigned long request, void *arg)
{
	return haiku_drm_ioctl(fd, request, arg);
}

/* drmOpen — not needed, crocus receives fd from the platform layer */
static inline int drmOpen(const char *name, const char *busid)
{
	(void)name; (void)busid;
	return haiku_drm_open();
}

static inline int drmClose(int fd)
{
	haiku_drm_close(fd);
	return 0;
}

/* Version info — crocus checks this for i915 identification */
typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

static inline drmVersionPtr drmGetVersion(int fd)
{
	(void)fd;
	static char name[] = "i915";
	static char date[] = "20260511";
	static char desc[] = "Haiku DRM shim for Intel Gen5";
	static drmVersion ver = {
		1, 6, 0,
		4, name, 8, date, 29, desc
	};
	return &ver;
}

static inline void drmFreeVersion(drmVersionPtr v)
{
	(void)v;
}

/* Syncobj stubs — crocus may probe for these */
static inline int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd; (void)flags; (void)handle;
	errno = ENOSYS;
	return -1;
}

#endif /* XF86DRM_H */
