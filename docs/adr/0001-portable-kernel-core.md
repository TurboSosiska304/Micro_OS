# ADR 0001 - Portable kernel with a port layer

* Status: accepted
* Date: 2026-10-21

> Russian version: [0001-portable-kernel-core_ru.md](0001-portable-kernel-core_ru.md)

## Context

The M5Stack StickC Plus2 is built around the ESP32. Existing development
environments (ESP-IDF, Arduino, MicroPython) give you "an application in C++ or
Python", not a kernel. To write a kernel with a scheduler, a VFS and system calls
we have to decide what to build on: an existing RTOS or from scratch.

The ESP32 is not "just a microcontroller": it has two cores, FreeRTOS, a WiFi
stack, ESP-NOW, TLS, a FAT filesystem, and all of that costs megabytes of flash
and tens of kilobytes of SRAM. Writing a kernel **from scratch**, dropping
FreeRTOS, means giving up WiFi, BLE, OTA and cryptography — giving up most of the
reason people buy M5Stack boards in the first place.

## Decision

The kernel is written **portable**, with a thin port layer:

* `components/kernel/` — the kernel in plain C11, without
  `#include "freertos/FreeRTOS.h"` and without `#include "esp_*.h"`. It compiles
  and is unit tested on Linux.
* `components/port_esp32/` — the implementation of the `kernel/port.h` contract for
  ESP32 (ESP-IDF). The only place where the kernel touches hardware.
* `ports/port_host/` — the host implementation: simulator and unit test bench.

System calls and the VFS live inside the kernel; "user" code calls them like
ordinary functions (the boundary is logical, there is no MMU).

## Consequences

**Good:**

* Kernel unit tests run in seconds, without flashing — a fast development loop.
* The kernel can be ported to another M5 board (Core2, Atom) or to an ESP32-S3 by
  writing a new port.
* Bugs in kernel logic are reproducible on the host.
* Doubts like "will this even work on an ESP32?" are cut off at build time: a
  stray `#include "esp_timer.h"` inside `components/kernel` immediately breaks the
  host build.

**Bad:**

* ESP-IDF facilities (spinlocks, timers, DMA) cannot be used directly inside the
  kernel — they need a wrapper in the port.
* The kernel/user boundary is not enforced by hardware: a bug in an "application"
  takes the whole system down. That is a deliberate trade for development speed.
* Some functions (critical sections) are stubs on the host: data races in the
  kernel are not reproduced there. Host scheduler tests are therefore limited.

## Alternatives

* **Writing our own kernel instead of FreeRTOS** — rejected: we lose WiFi/BLE/OTA/TLS
  and the amount of work grows several times over (own scheduler, SPI/I2C drivers,
  own bootloader and firmware).
* **Plain ESP-IDF + FreeRTOS without portability** — rejected: no fast development
  loop, no tests, and the kernel cannot be verified without hardware.
* **Real Linux on the ESP32 (esp-linux, NuttX)** — rejected: it requires an MMU,
  external SD/flash memory and does not fit into 8 MB with a 200 mAh battery. For
  the Plus2 that is a different class of device.
