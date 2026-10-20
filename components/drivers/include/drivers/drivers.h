/*
 * Micro OS — device drivers of the M5Stack StickC Plus2.
 *
 * Every driver registers a module in the kernel module registry, so that the
 * init process can check the state of the hardware after the boot.
 */
#ifndef MICRO_OS_DRIVERS_DRIVERS_H
#define MICRO_OS_DRIVERS_DRIVERS_H

#include "kernel/types.h"

/*
 * Register and initialise all the drivers. Called during the boot.
 * Returns 0 if every driver could be registered.
 */
int drivers_init(void);

#endif /* MICRO_OS_DRIVERS_DRIVERS_H */
