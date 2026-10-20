/*
 * Micro OS — platform port interface (the "hardware abstraction" contract).
 *
 * The kernel core never talks to hardware directly. Everything platform
 * specific goes through the functions declared here, which are implemented
 * once per platform:
 *
 *   components/port_esp32/  — ESP32 (ESP-IDF) implementation
 *   ports/port_host/        — Linux host / simulator implementation
 */
#ifndef MICRO_OS_KERNEL_PORT_H
#define MICRO_OS_KERNEL_PORT_H

#include "kernel/types.h"

/* Raw byte sink used by the log/console subsystem. */
void k_port_write(const char *data, k_size_t len);

/* Milliseconds since boot. */
k_u64_t k_port_uptime_ms(void);

/* Critical section (used to protect kernel data structures). */
void k_port_enter_critical(void);
void k_port_exit_critical(void);

/* Stop the CPU forever (used by panic and by idle). */
void k_port_halt(void);

/* Reset the board. */
void k_port_reset(void);

/* Short platform identifier: "esp32", "host", ... */
const char *k_port_name(void);

/* Human readable architecture name, e.g. "xtensa-esp32". */
const char *k_port_arch(void);

#endif /* MICRO_OS_KERNEL_PORT_H */
