/*
 * Micro OS — platform port interface (the "hardware abstraction" contract).
 *
 * The kernel core never talks to hardware directly. Everything platform
 * specific goes through the functions declared here, which are implemented
 * once per platform:
 *
 *   components/port_esp32/  — ESP32 (ESP-IDF) implementation
 *   ports/port_host/        — Linux host / simulator implementation
 *
 * The contract is split by subject, the way Linux keeps the generic
 * headers apart and lets each architecture fill in the parts it owns
 * (include/linux beside arch/<arch>/include/asm):
 *
 *   kernel/port/console.h  the bytes in and out
 *   kernel/port/time.h     the clock since the boot
 *   kernel/port/mem.h      the internal heap and the PSRAM
 *   kernel/port/lock.h     critical sections and recursive mutexes
 *   kernel/port/power.h    halt and reset
 *   kernel/port/kv.h       the store that survives a restart
 *   kernel/port/net.h      the sockets of TCP and UDP
 *   kernel/port/info.h     the name, the architecture, the contract version
 *
 * A source that needs one subject includes that header and nothing else;
 * this aggregate exists for the code that speaks to the port as a whole
 * (the boot, the tests). The version in kernel/port/info.h is the
 * vermagic of the pair: the kernel checks it at boot.
 */
#ifndef MICRO_OS_KERNEL_PORT_H
#define MICRO_OS_KERNEL_PORT_H

#include "kernel/port/console.h"
#include "kernel/port/info.h"
#include "kernel/port/kv.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/port/net.h"
#include "kernel/port/power.h"
#include "kernel/port/time.h"

#endif /* MICRO_OS_KERNEL_PORT_H */
