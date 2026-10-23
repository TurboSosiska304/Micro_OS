# Micro OS

A Linux-like kernel for the **M5Stack StickC Plus2** (ESP32-PICO-V3-02, 240 MHz,
2 MB PSRAM).

The goal is not "another ESP32 project" but an actual kernel: a task scheduler,
system calls, a VFS with `/proc` and `/dev`, a memory manager, device drivers and
an init process that starts a shell. All of it on bare metal without an MMU, with
a single ESP32 core, a 200 mAh battery and a 135x240 display.

> Russian version: [README_ru.md](README_ru.md)

[![CI](https://github.com/TurboSosiska304/Micro_OS/actions/workflows/ci.yml/badge.svg?branch=release_candidate)](https://github.com/TurboSosiska304/Micro_OS/actions/workflows/ci.yml)

## Status

| Phase | Content | State |
|-------|---------|-------|
| 0 | Skeleton, build system, log, board description, unit tests | done |
| 1 | Scheduler, tasks, timers, PIDs | done |
| 1F | Hardware doctor (modules and health checks) | done |
| 2 | VFS, MicroFS, shell | done |
| 3 | Syscall ABI, libc subset | in progress |
| 4 | Drivers: the WiFi station, the TFT console | in progress |
| 5 | init, configuration, applications, HTTP/NTP | planned |
| 6 | Release, documentation | planned |

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
idf.py -p /dev/ttyACM0 flash monitor
```

Or through the Makefile: `make idf-build`, `make idf-flash`, `make idf-monitor`.

The board needs to be reachable by the current user. On most distributions the
serial device belongs to the `uucp` group:

```bash
sudo gpasswd -a $USER uucp   # then log out and back in
```

### Checking the kernel on the device

The init process runs a self test on the real hardware: three tasks with
different priorities sleep, yield and wake up, and the init task verifies the
result and prints a `ps` like table of the task list.

```bash
tools/device-test.sh              # build, flash and check automatically
tools/device-test.sh /dev/ttyACM0 # with an explicit port
tools/console.py                  # just read the console
tools/console.py /dev/ttyACM0 20 --reset --send "modules"
```

The script exits with `RESULT: PASS` when the self test succeeded.

### The hardware doctor

After the self test the init process walks through the registered modules and
checks them one by one. The result is a report:

```
module health report:
  module     state     detail
  psram      ok        2048 KB
  flash      ok        8 MB
  imu        ok        acc -16,-2952,16728 (raw)
  rtc        degraded  01:00:00 (no clock)
  display    ok        135x240, fb 63 KB
  input      ok        A=up B=up C=up
  buzzer     ok        passive, 2 kHz via LEDC
  led/ir     ok        G19, shared with the IR emitter
  battery    ok        4176 mV (raw 2088 mV)
  wifi       degraded  f4:65:0b:32:00:c0 idle
  mic        skip      SPM1423 MEMS microphone (I2S)
11 module(s), 0 problem(s)
```

`ok` works, `degraded` works with limitations (no access point, no clock
without a backup battery), `fail` does not work and `skip` is not implemented
yet. The command `modules` repeats the check at any time.

### The shell

The init process starts the shell, which speaks a small subset of the Linux
commands:

| Command | Meaning |
|---|---|
| `help` | list the commands |
| `ps` | the task list |
| `uptime` | the time since the boot |
| `uname` | the name and the version of the kernel |
| `free` | the memory usage |
| `dmesg` | the kernel log |
| `modules` | check the hardware modules again |
| `memtest` | measure the memory and its speed |
| `i2c` | scan the I2C bus |
| `date` | the time of the RTC |
| `wifi` | the WiFi station: `status`, `scan`, `connect`, `join`, `disconnect` |
| `mfetch` | the report about the system with the logo (like fastfetch) |
| `echo`, `sleep`, `clear`, `reboot` | the usual suspects |

The command `mfetch` is the analog of fastfetch: the logo of the system on
the left and a short report about the board on the right. The drawing comes
from [logos.md](logos.md), the size "L". It is a drawing for the serial
console: the screen of the board has a 5x7 font that stops at `0x5F` and
shows the box drawing characters as `?`, while the report itself is plain
text and is readable on both consoles. `mfetch color` paints the logo.

```
microos> mfetch
███╗   ███╗ ██████╗ ███████╗  OS           MicroOS 0.1.0
████╗ ████║██╔═══██╗██╔════╝  Board        M5Stack StickC Plus2
██╔████╔██║██║   ██║███████╗  SoC          ESP32-PICO-V3-02
██║╚██╔╝██║██║   ██║╚════██║  Arch         xtensa-esp32
██║ ╚═╝ ██║╚██████╔╝███████║  Display      1.14" TFT 135x240 (ST7789v2)
╚═╝     ╚═╝ ╚═════╝ ╚══════╝  Memory       heap 384/512 KB, psram 1024 KB free
                              Uptime       42 s, 6 task(s)
                              Shell        28 command(s)
```

### The WiFi station

The radio of the board is a driver of its own: a portable core that holds the
state of the station and the radio of the ESP32 that talks to the hardware.
The network of the last time is remembered in the NVS, so the board joins it
on its own after a restart.

```
microos> wifi scan
3 network(s):
home            -42 dBm  ch  6  locked
guest           -70 dBm  ch 11  open
microos> wifi connect home secret
connecting to home
microos> wifi status
home aa:bb:cc:dd:ee:ff, ip 192.168.1.42, -42 dBm, ch 6
saved network: home
```

The decision about the shape of the driver is recorded in
[docs/adr/0003-wifi-driver.md](docs/adr/0003-wifi-driver.md).

### Ready to flash binaries

The `dist/` folder holds the built firmware: the application, the bootloader,
the partition table, a flash script and the sums of the files. The folder is
not in the repository, it is a product of the build: every CI run uploads it
as the artifact `firmware`, and a tag of the form `vX.Y.Z` publishes it as a
release. Download the folder, connect the board and run `./flash.sh` — no
sources, no ESP-IDF and no toolchain needed.

## Continuous integration

Every push to `release_candidate` builds the firmware for the ESP32
(ESP-IDF v5.3) and checks the integrity of the repository: the headers of the
images, the partition table against `partitions.csv`, the git objects and the
formatting of the sources. The tests of the code itself are run by the
contributor before a push; after the first release they get their own CI/CD.

```bash
make ci          # the whole list of checks, locally
```

A tag of the form `vX.Y.Z` publishes the bundle as a GitHub release. The
details are in [docs/ci.md](docs/ci.md).

## Repository layout

```
Micro_OS/
├── components/
│   ├── kernel/         portable kernel: boot, log, panic, version, tasks, sched
│   ├── board/          hardware description
│   ├── wifi/           the WiFi station: the portable core and the radio
│   └── port_esp32/     ESP32 port
├── ports/port_host/    Linux port + simulator
├── main/               firmware entry point (app_main)
├── tests/              unit tests (own minimal framework, no dependencies)
├── tools/              console, bundle, the smoke test, the integrity check
├── docs/               architecture, roadmap, hardware notes, ADRs
├── .github/workflows/  the autobuild of the release candidate
├── CMakeLists.txt      dual mode build: ESP-IDF or host
└── Makefile            convenience targets: test, sim, ci, format, idf-*
```

## Development rules

* C11, `-Wall -Wextra -Werror`, 4 spaces, Linux/BSD mixed style (see `.clang-format`).
* One logical change per commit; messages in the Linux style:
  `subsys: add vfs mount table`, `drivers: fix ir carrier frequency`.
* Any new kernel behaviour is covered by a test in `tests/`.
* Formatting: `make format`.
* Before you push: `make ci` (the build, the tests, the smoke test and the
  integrity check).

Details: [CONTRIBUTING.md](CONTRIBUTING.md).

## Known limitations

* **No MMU.** The kernel/user split is logical, not hardware enforced: there is no
  memory protection and no real address space isolation.
* **8 MB flash / 2 MB PSRAM.** There is one application and no room for an
  update over the air: the kernel takes 2 MB (`factory`) and the rest of the
  flash (5.75 MB) belongs to MicroFS. The whole layout is in
  [partitions.csv](partitions.csv).
* **The ESP32-PICO-V3-02 has no PMIC.** Power and sleep are handled by the ESP32
  itself: `HOLD` (G4) must be driven high after a wake-up or the board powers off.
* **One power button plus two user buttons** (A, B), and a third one (C) on Plus2.

## License

MIT — see [LICENSE](LICENSE).
