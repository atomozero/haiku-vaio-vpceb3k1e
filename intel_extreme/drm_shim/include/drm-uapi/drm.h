/*
 * drm.h — Minimal DRM base header for Haiku shim.
 */
#ifndef DRM_H
#define DRM_H

#include <stdint.h>

/* DRM_IOCTL base — on Linux this is 'd', here unused since
 * we dispatch by request number directly. */
#define DRM_IOCTL_BASE 'd'

#endif /* DRM_H */
