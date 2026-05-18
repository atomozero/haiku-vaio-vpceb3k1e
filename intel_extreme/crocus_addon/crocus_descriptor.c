/*
 * crocus_descriptor.c — Provides crocus_driver_descriptor for Haiku addon.
 *
 * Mesa's DRM_DRIVER_DESCRIPTOR macro expands to a const struct definition.
 * The Haiku target must compile this with -DGALLIUM_CROCUS so drm_helper.h
 * generates the real descriptor (with create_screen function and driconf).
 */

#include <stdio.h>

/* Must be defined BEFORE including drm_helper.h */
#define GALLIUM_CROCUS

#include "target-helpers/drm_helper.h"
