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
* [ ] `v0.4.0`: the release candidate accepted (the tag is cut when it ships)

## P3 - The wearable (planned)

* [ ] The RTC alarm and deep sleep, the wake from EXT1
* [x] `drv_mic` (SPM1423, PDM RX on I2S, 16 kHz)
* [ ] A simple sound detector on top of `drv_mic`
* [ ] `drv_ir`: transmit and receive on G19, shared with the LED
* [ ] A factory reset, the enclosure and display modes

## P4 - Updates (planned)

* [ ] OTA with two application slots and a rollback: the flash has to be
      repartitioned first, which is a data-loss operation on the volume

## P5 - Applications (not in this tree)

* [n] The sensor logger, the clock, the weather station: a repository of their
      own. A board with 8 MB of flash does not carry programs nobody asked
      for, and the kernel has to stay one kernel.
