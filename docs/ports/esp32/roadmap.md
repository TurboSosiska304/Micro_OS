# The ESP32 port: the board

This is the plan and the notes of **one port** of Micro OS: the M5Stack
StickC Plus2 (ESP32-PICO-V3-02, 240 MHz, 2 MB PSRAM, 8 MB flash, 135x240
TFT). The kernel itself is planned in [../../roadmap.md](../../roadmap.md);
this document is the platform - the board, its drivers, its services and its
release.

The port is built with `make idf-build` and checked on the board with
`tools/device-test.sh`.

## P0 - The board (done)

* [x] Display (ST7789v2), input (A/B/C), buzzer, LED and the IR pin,
      IMU (MPU6886), RTC (BM8563), battery, WiFi radio
* [x] The hardware doctor: a module per piece, a health check and a report
* [x] The console on the TFT, the 5x7 font, the renderer of the panel
* [x] The microphone (SPM1423, PDM RX on I2S, 16 kHz, RMS and peak in
      the doctor)

The pins, the chips and the quirks of the board are in
[hardware.md](hardware.md).

## P1 - Services on the board (done)

* [x] init and the scripts of `/etc/init.d`, the configuration in the NVS
* [x] WiFi: the station joins the saved network on its own
* [x] telnet on port 23, the HTTP client, the clock of NTP

## P2 - The release (done)

* [x] The process of a release, the tags, the cloud build, the bundle
* [x] `v0.3.1`: the first release of the kernel with ports
* [x] `v0.4.0-rc1`: phase 7, programs and the permissions of a file
* [x] `v0.4.0-rc2`: the debt pass - entry into the kernel, HTTP page, microphone, fsck
* [ ] `v0.4.0`: the release candidate accepted (the tag is cut when it ships)

## P3 - The wearable (planned)

* [x] The RTC alarm and deep sleep, the wake from EXT1
* [x] `drv_mic` (SPM1423, PDM RX on I2S, 16 kHz)
* [ ] A simple sound detector on top of `drv_mic`
* [ ] `drv_ir`: transmit and receive on G19, shared with the LED
* [x] A factory reset (the volume and the store)
* [ ] The enclosure and display modes

## P4 - Programs (for 1.0)

* [x] MOX1 version 2: the field of the machine in the header (x86-64, Xtensa),
      `mkprog --arch`, and an image of another machine is `ENOEXEC` before it
      runs (the contract of the port is 1.8)
* [ ] A `progs` partition of 256 KiB, mapped as instructions with
      `esp_partition_mmap`; a `.mox` at a 64 KiB boundary. The first loader
      does not wait for it: the images travel in the firmware and are written
      to `/bin` of MicroFS at the boot (ADR 0018 §2)
* [x] The spike of the door: code in the flash, mapped as instructions and
      called, talks to a kernel function through a pointer; `call0` and the
      `syscall` instruction are rejected (ADR 0018)
* [x] The two windows of the loader: the text in the 8 KiB of IRAM below
      `0x400A0000`, the data and the bss in the 8 KiB above `0x3FFCC000`,
      and the linker refuses a kernel that grows into either
      (`exec_windows.ld`)
* [x] A build of a program for Xtensa with the windowed ABI, its linker
      script, the same `_start` and library, and the trampoline of the port as
      the door to `ksys_call` (`programs/hello.c`, `programs/cat.c`)
* [x] `k_port_exec_supported()` is true, `execve` runs the image, and
      `tools/device-test.sh` runs `/bin/hello` on the board and checks the
      `ENOEXEC` of an x86-64 image
* [x] The price in the docs: no MMU, no address space, a bad pointer can take
      the machine down (ADR 0018 §4, `docs/debts.md`)

After 1.0 the port is a **compatibility port**: it builds and passes the device
test, and receives only the features that are cheap on this board - memory,
the concurrency FreeRTOS gives for free, drivers, the console. A compiler of
our own, the higher half, NX and full SMP are not for it. ADR 0018.

## P5 - Updates (planned)

* [ ] OTA with two application slots and a rollback: the flash has to be
      repartitioned first, which is a data-loss operation on the volume

## P6 - Applications (not in this tree)

* [n] The sensor logger, the clock, the weather station: a repository of their
      own. A board with 8 MB of flash does not carry programs nobody asked
      for, and the kernel has to stay one kernel.
