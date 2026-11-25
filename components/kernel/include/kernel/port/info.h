/*
 * Micro OS — platform port: the identity and the version.
 *
 * The version of the contract is the "vermagic" of Micro OS: the kernel
 * checks it at boot, so a port written for another generation of the
 * interface is named in plain words instead of misbehaving quietly.
 *
 * A major mismatch means the two disagree about the meaning of a call;
 * a minor one means the port is older than the kernel and lacks the calls
 * that were added since. Linux solves the same problem with vermagic in
 * include/linux/vermagic.h, checked when a module is loaded.
 */
#ifndef MICRO_OS_KERNEL_PORT_INFO_H
#define MICRO_OS_KERNEL_PORT_INFO_H

#include "kernel/types.h"

/* The contract the kernel itself speaks. */
#define KERNEL_PORT_API_VERSION_MAJOR 1u
#define KERNEL_PORT_API_VERSION_MINOR 15u

/* Short platform identifier: "esp32", "host", ... */
const char *k_port_name(void);

/* Human readable architecture name, e.g. "xtensa-esp32". */
const char *k_port_arch(void);

/* The contract the port was written for (checked by the boot sequence). */
unsigned k_port_api_major(void);
unsigned k_port_api_minor(void);

#endif /* MICRO_OS_KERNEL_PORT_INFO_H */
