# Micro OS roadmap

> Russian version: [roadmap_ru.md](roadmap_ru.md)

Every phase ends with a working state on the device and green tests.

## Phase 0 - Skeleton (done)

* [x] Repository, directory layout, licence, commit rules
* [x] Dual mode build: ESP-IDF (device) and host (simulator + tests)
* [x] Portable kernel: types, compile time config, levelled log with a ring
      buffer, panic handler, version and build identification
* [x] Boot sequence with ordered subsystem initialisation and error reporting,
      mirroring the Linux initcall model
* [x] Board description (SoC, flash, PSRAM, display, IMU, RTC, power)
* [x] ESP32 port (UART output, time source, critical sections) and a host port
* [x] Unit tests (own framework, no external dependencies)
* [x] Documentation: README, CONTRIBUTING, architecture, hardware notes, ADRs

**Deliberately out of scope:** display and button initialisation require ESP-IDF.

## Phase 1 - Tasks and scheduler

* [x] `struct task`, states, priorities, PID allocation
* [x] Context switching (FreeRTOS substrate), `k_sched_yield`, `k_sched_sleep_ms`
* [x] Run queues per priority level, sleep queue, 1 ms tick
* [x] `init` as PID 1, spawning "user" tasks
* [x] Tests: task lifecycle, priorities, ready queue, sleeping, ticking
* [ ] `/proc/<pid>`, per task file descriptors and working directory
* [ ] `fork`/`exec`/`wait` semantics on top of the task model
* [ ] Round-robin preemption with a time slice (Linux `SCHED_RR` style)

## Phase 2 - VFS, procfs, devfs, shell

* [ ] `inode` / `dentry` / `file_operations`
* [ ] In-RAM files (`/etc`, `/tmp`), mounting
* [ ] `/proc`: `version`, `uptime`, `meminfo`, `tasks`, `self/`
* [ ] `/dev`: `tty0` (console on UART and TFT)
* [ ] Shell with commands: `ls`, `cat`, `echo`, `ps`, `free`, `dmesg`, `uname`,
      `mount`, `uptime`, `reboot`
* [ ] Command history, `help`, tab completion

## Phase 3 - Memory and the syscall ABI

* [ ] `kmm_alloc`/`kmm_free`, slabs, allocation from PSRAM
* [ ] System call table, uniform ABI
* [ ] Minimal libc on top of the kernel: `open/read/write/close/malloc/printf`
* [ ] Permission model and "file descriptors"

## Phase 4 - Drivers

* [ ] `drv_framebuffer`: ST7789v2, 135x240, rotation, text and graphics output
* [ ] `drv_input`: A/B/C buttons, debounce, input events
* [ ] `drv_buzzer`, `drv_led` (IR+LED share G19), `drv_ir` (transmit and receive)
* [ ] `drv_imu` (MPU6886), `drv_rtc` (BM8563, deep-sleep wake)
* [ ] `drv_mic` (SPM1423) — a simple sound/noise detector
* [ ] Console on TFT and UART at the same time

## Phase 5 - Services and applications

* [ ] Init process: starting daemons from `/etc/init.d`
* [ ] Configuration from `/etc`, persisted in NVS
* [ ] WiFi, HTTP/MQTT client, NTP based on the RTC
* [ ] Applications: sensor logger, IR remote, clock, weather station

## Phase 6 - Release

* [ ] OTA updates (`app0`/`app1`), rollback after a failed boot
* [ ] Settings reset, `factory reset`
* [ ] Enclosure/display modes: "clock", "remote", "logger"
* [ ] Release process, tags, changelog
