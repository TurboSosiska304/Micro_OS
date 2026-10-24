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
* [x] Time slice (SCHED_RR style), configurable quantum
* [x] Parent/child relations, `task_wait()`, `task_wait_any()`
* [x] File descriptor table and working directory per task
* [x] `/proc/<pid>` (belongs to phase 2)
* [ ] A real `fork()` — a copy of the address space (needs our own context
      switch, see ADR 0002). Until then `fork()` is a placeholder: the child
      shares the memory of the parent and re-runs its program from the start
      (see "Known limitations" in the README), to be fixed right after the
      first release

## Phase 1F - The hardware doctor (done)

The init process walks through the registered modules and checks them one by
one. The result is printed as a report: `ok`, `degraded`, `fail` or `skip`.

* [x] Module registry in the kernel (`kernel/kmod`) with init and health check
* [x] Display (ST7789v2, framebuffer in PSRAM, test pattern)
* [x] Buttons A, B and C
* [x] Buzzer (LEDC, 2 kHz)
* [x] Red LED and IR emitter (shared pin G19)
* [x] IMU (MPU6886 on I2C)
* [x] RTC (BM8563 on I2C)
* [x] Battery voltage (ADC on G38)
* [x] WiFi radio
* [x] PSRAM and flash
* [x] A `doctor` command that repeats the check at any time (as a comand "modules")

## Phase 2 - VFS, MicroFS, shell (done)

* [x] Shell: a portable command interpreter with a command registry
* [x] Built in commands: `help`, `uname`, `uptime`, `ps`, `echo`, `sleep`,
      `dmesg`, `free`, `clear`, `reboot`, `modules`, `memtest`, `i2c`
* [x] `memtest`: the size of the internal heap and of the PSRAM, and the
      speed of both (fill, checksum, copy) with a verification of the data
* [x] `mfetch`: the report about the system with the logo of `logos.md`
      (the analog of fastfetch, the logo on the left and the report on
      the right)
* [x] The console of the ESP32 port (the UART driver makes the input work)
* [x] MicroFS: the filesystem of the board, as described in
      [docs/memory.md](memory.md): the DATA, META and INFO sections, the
      addresses of the sections are computed from the size of the volume
* [x] The VFS: the tree of the directories, the filesystems are mounted at
      a point of it
* [x] The file commands: `ls`, `cat`, `write`, `mkdir`, `rm`, `cd`, `pwd`,
      `df`, `mount`
* [x] The volume of the flash: the partition of 5.75 MB, the data of the
      user survives the restarts
* [x] `/proc`: `version`, `uptime`, `meminfo`, `tasks`, `modules`
* [x] `/dev`: `tty0` (the console) and `null`
* [x] The console on the display: the output of the shell is mirrored to the
      screen of the board, the font is 5x7
* [x] The help is short: one line per command, `help <command>` tells more
* [x] The log of the boot goes to `/log/start.log`, the console stays readable
* [x] The terminal echoes the input, so that the dialogue is readable in the
      interactive console
* [x] The image of the display: the ST7789V2 has 240x320 of memory, the
      glass of the 1.14" panel shows only a part of it. The offset can be
      changed while the board runs: `display <x> <y>`
* [x] The terminal repeats every character while it is typed, the backspace
      erases it
* [x] `/proc/self` and a directory per task with a `status` file
* [n] Command history, tab completion (not needed for now)

## Phase 3 - Memory and the syscall ABI (done)

* [x] The ABI of the system calls: the numbers are a contract, they are
      written down in [docs/syscall-abi.md](syscall-abi.md) and covered by
      a test
* [x] The calls of the files: open, read, write, close, stat, lseek,
      unlink, mkdir, readdir, getpid, getcwd, ioctl, yield, sleep, uptime
* [x] The tests work with a corpus of real numbers: the same data has to
      give the same result no matter how it is broken into pieces
* [x] The process part of the ABI: fork, getppid, waitpid. The memory
      of the parent is not copied, the tasks share it, like CLONE_VM.
      The library gives fork() the shape of POSIX - 0 in the child, the
      number of the child in the parent - but the child re-runs the
      program of the parent from its start (a real fork waits for the
      first release)
* [x] The libc over the ABI: printf, open, read, write. The library is
      checked against the library of the system with the real numbers of the
      corpus
* [x] `kmm_alloc`/`kmm_free`, allocation from the PSRAM, the slabs for the
      small blocks
* [x] The memory of the programs: malloc, free, realloc and calloc over the
      ABI
* [x] System call table, uniform ABI
* [x] Minimal libc over the ABI: `open/read/write/close/printf`. The memory
      (`malloc`) is still missing
* [x] Permission model and "file descriptors": a descriptor remembers the
      access it was opened with, and the kernel refuses to read a write-only
      descriptor or to write a read-only one with `EBADF`. The rule lives at
      the edge of the system, so every filesystem answers the same way.
      Per-file owners and mode bits wait for the first release - there is a
      single user today (see "After the first release")

## Phase 4 - Drivers

* [ ] `drv_framebuffer`: ST7789v2, 135x240, rotation, text and graphics output
* [ ] `drv_input`: A/B/C buttons, debounce, input events
* [x] `drv_buzzer`: the passive buzzer on G2, a `beep` command for the user
* [ ] `drv_led`: the red LED and the IR emitter on G19 (still only a health check)
* [ ] `drv_ir`: transmit and receive (the driver itself is missing)
* [ ] `drv_imu` (MPU6886), `drv_rtc` (BM8563, deep-sleep wake)
* [x] `drv_wifi`: the station, the scan, the network of the last time is
      remembered in the NVS (see [ADR 0003](adr/0003-wifi-driver.md))
* [x] Console on TFT and UART at the same time

## Phase 5 - Services and applications

* [ ] Init process: starting daemons from `/etc/init.d`
* [ ] Configuration from `/etc`, persisted in NVS
* [x] WiFi: the station joins the network of the last time on its own
* [ ] HTTP client, NTP based on the RTC
* [ ] Applications: sensor logger, IR remote, clock, weather station

## Phase 6 - Release

* [x] Autobuild of the release candidate (GitHub Actions): the firmware and
      the integrity check, see [ci.md](ci.md)
* [n] OTA updates (`app0`/`app1`), rollback after a failed boot (no space
      in the flash for a second image)
* [n] Enclosure/display modes: "clock", "remote", "logger" (not needed)
* [ ] Release process, tags, changelog

## After the first release

Deferred, not forgotten:

* [ ] `execve`: it needs a format of the programs
* [ ] Per-file permissions: mode bits and owners in MicroFS (today the model
      is one user and the access a descriptor was opened with, see the
      permission model of phase 3)
* [ ] Microphone (SPM1423, I2S) and `drv_mic` — a simple sound/noise detector
* [ ] MQTT client (HTTP and NTP stay in phase 5)
* [ ] Settings reset, `factory reset`
