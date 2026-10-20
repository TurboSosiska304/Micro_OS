/*
 * Micro OS — ESP32 platform port.
 *
 * Implements the kernel/port.h contract for the M5StickC Plus2 (ESP32).
 * All hardware access lives behind this layer, the kernel core stays portable.
 */
#ifndef MICRO_OS_PORT_ESP32_H
#define MICRO_OS_PORT_ESP32_H

#include "kernel/types.h"

/* Bring up the console (UART0 by default). Safe to call more than once. */
void port_esp32_console_init(void);

#endif /* MICRO_OS_PORT_ESP32_H */
