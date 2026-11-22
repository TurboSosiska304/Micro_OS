/*
 * Micro OS — ESP32 platform port.
 *
 * Implements the kernel/port.h and kernel/port_task.h contracts for the
 * M5StickC Plus2 (ESP32). All hardware access lives behind this layer, the
 * kernel core stays portable.
 */
#ifndef MICRO_OS_PORT_ESP32_H
#define MICRO_OS_PORT_ESP32_H

#include "esp_err.h"

#include "kernel/types.h"

/* Bring up the console (UART0 by default). Safe to call more than once. */
void port_esp32_console_init(void);

/* Create the kernel lock. Must be called before the first task is created. */
void port_esp32_init(void);

/* Register the block devices of the board (the partition of MicroFS). */
void port_esp32_blockdev_init(void);

/*
 * Bring up the radio of the WiFi station and install it into the portable
 * core (components/wifi). Safe to call more than once.
 */
void port_esp32_wifi_init(void);

/* Start the 1 ms time base (calls k_sched_tick()). */
esp_err_t port_esp32_start_tick(uint32_t period_ms);

/*
 * Check the signature and the version of the application before the kernel
 * mounts a filesystem (ADR 0020). A verified image continues; an image that
 * did not pass continues only when CONFIG_MICRO_OS_REQUIRE_SIGNATURE is off,
 * and then the console carries the warning. The anti-rollback of the NVS
 * follows the same policy (CONFIG_MICRO_OS_ANTIROLLBACK). 0 when the boot may
 * continue, -1 when it must stop.
 */
int port_esp32_image_check(void);

/*
 * The door of a program (ADR 0018). The program is entered with the address
 * of this function; every wrapper of components/libc/user calls it with the
 * number and the six arguments of the syscall ABI, and it answers what
 * ksys_call() answers. The shape is the one of ksys_call() on purpose: the
 * door adds nothing to the argument and nothing to the answer.
 */
long port_mox_syscall(long number, long a0, long a1, long a2, long a3, long a4, long a5);

/*
 * Put the programs that ship with the firmware into /bin and run one of them
 * through execve. Called by the self test after the filesystem is mounted; a
 * failure is a line on the console and not a reason to stop the boot.
 */
int port_esp32_programs_boot(void);

#endif /* MICRO_OS_PORT_ESP32_H */
