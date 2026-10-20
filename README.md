# Micro OS

A Linux-like kernel for the **M5Stack StickC Plus2** (ESP32-PICO-V3-02, 240 MHz,
2 MB PSRAM).

The goal is not "another ESP32 project" but an actual kernel: a task scheduler,
system calls, a VFS with `/proc` and `/dev`, a memory manager, device drivers and
an init process that starts a shell. All of it on bare metal without an MMU, with
a single ESP32 core, a 200 mAh battery and a 135x240 display.

> Russian version: [README_ru.md](README_ru.md)

## Status

| Phase | Content | State |
|-------|---------|-------|
| 0 | Skeleton, build system, log, board description, unit tests | done |
| 1 | Scheduler, tasks, timers, PIDs | in progress |
| 2 | VFS, `/proc`, `/dev`, shell | planned |
| 3 | Memory manager, syscall ABI, libc subset | planned |
| 4 | Drivers: TFT, buttons, buzzer, IMU, RTC, IR, microphone | planned |
| 5 | init, configuration, applications, WiFi/MQTT | planned |
| 6 | OTA, release, documentation | planned |

See [docs/roadmap.md](docs/roadmap.md) for the full plan and
[docs/architecture.md](docs/architecture.md) for the design.

## The key idea: a portable kernel

The kernel **knows nothing about hardware**. Everything platform specific is
behind a port:

```
components/kernel/    <- plain C11, zero ESP-IDF dependencies
components/board/     <- board description
components/port_esp32/<- ESP32 implementation of the port (ESP-IDF)
ports/port_host/      <- Linux implementation (simulator + unit tests)
```

Consequence: the kernel compiles and its tests run on a normal PC in seconds. The
edit/compile/run loop does not involve flashing a device, while the firmware
itself stays a plain ESP-IDF project.

## Build and run

### Host (simulator + tests) — works out of the box

```bash
make test     # build and run the unit tests
make sim      # run the kernel simulator on Linux
```

Or directly:

```bash
cmake -B build-host -S . -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
./build-host/ports/port_host/microos_sim
```

### Device (ESP-IDF)

Requires ESP-IDF v5.x:

```bash
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
. ~/esp/esp-idf/export.sh

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Or through the Makefile: `make idf-build`, `make idf-flash`, `make idf-monitor`.

## Repository layout

```
Micro_OS/
├── components/
│   ├── kernel/         portable kernel: boot, log, panic, version, tasks, sched
│   ├── board/          hardware description
│   └── port_esp32/     ESP32 port
├── ports/port_host/    Linux port + simulator
├── main/               firmware entry point (app_main)
├── tests/              unit tests (own minimal framework, no dependencies)
├── docs/               architecture, roadmap, hardware notes, ADRs
├── CMakeLists.txt      dual mode build: ESP-IDF or host
└── Makefile            convenience targets: test, sim, format, idf-*
```

## Development rules

* C11, `-Wall -Wextra -Werror`, 4 spaces, Linux/BSD mixed style (see `.clang-format`).
* One logical change per commit; messages in the Linux style:
  `subsys: add vfs mount table`, `drivers: fix ir carrier frequency`.
* Any new kernel behaviour is covered by a test in `tests/`.
* Formatting: `make format`.

Details: [CONTRIBUTING.md](CONTRIBUTING.md).

## Known limitations

* **No MMU.** The kernel/user split is logical, not hardware enforced: there is no
  memory protection and no real address space isolation.
* **8 MB flash / 2 MB PSRAM.** Kernel, applications and data share one flash
  (OTA slots `app0`/`app1` plus `spiffs`).
* **The ESP32-PICO-V3-02 has no PMIC.** Power and sleep are handled by the ESP32
  itself: `HOLD` (G4) must be driven high after a wake-up or the board powers off.
* **One power button plus two user buttons** (A, B), and a third one (C) on Plus2.

## License

MIT — see [LICENSE](LICENSE).
