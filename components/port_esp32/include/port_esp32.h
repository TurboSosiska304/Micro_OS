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

#endif /* MICRO_OS_PORT_ESP32_H */
