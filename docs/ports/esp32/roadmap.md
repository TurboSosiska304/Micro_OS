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

## P3 - The wearable (in work)

* [x] Deep sleep with HOLD (G4) held, the wake from the timer and from EXT1
      (BUTTON C), and the reason of the boot in the log
* [x] The RTC alarm (BM8563) as a wake source: the chip has no line to G35,
      so a sleep sets its own timer for the moment of the alarm and the flag
      of the clock confirms it (`rtc: the alarm woke the board`); the alarm
      survives a restart and a factory reset, because it lives in the clock
* [x] `power sleep <seconds>`, `power sleep alarm` and `power off`; the
      console says what the wake will be
* [x] `drv_mic` (SPM1423, PDM RX on I2S, 16 kHz)
* [x] A sound detector on top of `drv_mic`: RMS against a threshold, two
      windows to begin an event and four to end it, `sound listen`,
      `sound threshold`, `sound watch on|off`, a module of the doctor
* [x] `drv_ir`: NEC on G19 with the carrier of the RMT, the arbitration of
      the pin with the LED, `ir send`, `ir frame`; reception on G26 for an
      external demodulating element (the board has no receiver, hardware.md)
* [x] A factory reset (the volume and the store, the defaults of `/etc`
      written back, the alarm of the clock kept)
* [ ] The enclosure and display modes

## P4 - Programs (done)

* [x] MOX1 version 2: the field of the machine in the header (x86-64, Xtensa),
      `mkprog --arch`, and an image of another machine is `ENOEXEC` before it
      runs (the contract of the port is 1.8)
* [x] A `progs` partition of 132 KiB at `0x250000`, 64 KiB aligned, with
      two 64 KiB slots and a directory of name, offset and size in its last
      4 KiB; the boot mounts it read-only at `/bin` and the images are no
      longer blobs of the firmware (`tools/progs_image.py`, ADR 0022). The
      loader copies the bytes into the windows; the mapping of flash as
      instructions is a later loader and the table keeps the address it needs
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

## P5 - Updates (done over USB; the network is open)

* [x] Two equal application slots of 1.125 MiB (`ota_0`, `ota_1`), otadata
      and a 5.56 MiB MicroFS; the one-time move of a board of 1.0 and its
      warning (ADR 0022)
* [x] The update of the inactive slot over USB (`tools/ota_usb.py`), the
      state `NEW` in otadata, the confirmation at the shell
      (`esp_ota_mark_app_valid_cancel_rollback()`) and the rollback of a boot
      that never confirms itself (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`)
* [x] `DEVICE_OTA=1 tools/device-test.sh` on the board: a good update to
      `ota_1`, back to `ota_0`, and a wrong trailer that is refused and rolls
      back
* [ ] An update over the network (HTTP) into the inactive slot, with the same
      signature and rollback; the kernel's HTTP client is not a stream for
      `esp_ota_write()` yet (ADR 0022, `docs/debts.md`)

## P6 - Applications (not in this tree)

* [n] The sensor logger, the clock, the weather station: a repository of their
      own. A board with 8 MB of flash does not carry programs nobody asked
      for, and the kernel has to stay one kernel.
