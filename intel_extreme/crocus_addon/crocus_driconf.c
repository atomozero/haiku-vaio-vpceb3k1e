/*
 * crocus_driconf.c — Generate crocus driconf options array.
 * Compiled separately with Mesa headers, provides the driconf
 * for crocus_driver_descriptor.
 */

#include "util/driconf.h"

/* Include crocus driconf option definitions */
static const driOptionDescription crocus_driconf_options[] = {
    #include "crocus/driinfo_crocus.h"
};

const void* crocus_get_driconf(void) {
    return crocus_driconf_options;
}

unsigned crocus_get_driconf_count(void) {
    return sizeof(crocus_driconf_options) / sizeof(crocus_driconf_options[0]);
}
