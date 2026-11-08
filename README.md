# Micro OS

A Linux-like kernel for the **M5Stack StickC Plus2** (ESP32-PICO-V3-02, 240 MHz,
2 MB PSRAM).

The goal is not "another ESP32 project" but an actual kernel: a task scheduler,
system calls, a VFS with `/proc` and `/dev`, a memory manager, device drivers and
an init process that starts a shell. All of it on bare metal without an MMU, on
top of FreeRTOS (which owns both ESP32 cores), with a 200 mAh battery and a
135x240 display.

> Russian version: [README_ru.md](README_ru.md)

## Status

| Phase | Content | State |
|-------|---------|-------|
| 0 | Skeleton, build system, log, board description, unit tests | done |
| 1 | Scheduler, tasks, timers, PIDs | done |
| 1F | Hardware doctor (modules and health checks) | done |
| 2 | VFS, MicroFS, shell | done |
| 3 | Syscall ABI, libc subset, memory manager (slabs) | done |
| 4 | Drivers: the WiFi station, the TFT console, input, sensors | done |
| 5 | init, configuration, HTTP/NTP, applications | in progress |
| 6 | Release, documentation | in progress |

Phase 5 is partly done: the WiFi station and telnet (the shell over the air)
work, the init daemons, the `/etc` configuration, HTTP/NTP and the demo
applications are still ahead. Phase 6 has the release process and the cloud
build; OTA is not possible with the current flash layout.

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

Requires ESP-IDF v5.3 (the version the cloud build uses):

```bash
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
. ~/esp/esp-idf/export.sh

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The Makefile finds ESP-IDF by itself, in this order: `$IDF_PATH`, a checkout
next to the repository (`../esp-idf`), then `~/esp/esp-idf`. So the same build
is one command, or point it at another `export.sh` explicitly:

```bash
make idf-build
make idf-build IDF_EXPORT=/path/to/esp-idf/export.sh
```

The host build needs `cmake`, `ninja` and a C compiler. ESP-IDF brings its own
cross toolchain and Python environment, so no second compiler is needed for the
device build.

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
  battery    ok        4176 mV, 98% (raw 2088 mV)
  wifi       degraded  f4:65:0b:32:00:c0 idle
  telnet     ok        port 23, no auth
  mic        skip      SPM1423 MEMS microphone (I2S)
12 module(s), 0 problem(s)
```

`ok` works, `degraded` works with limitations (no access point, no clock
without a backup battery), `fail` does not work and `skip` is not implemented
yet. The command `modules` repeats the check at any time.

### The init scripts

The init process treats every file of `/etc/init.d` as a daemon of the board:
a script is a list of shell commands, one per line, empty lines and lines that
start with `#` are skipped, and each script runs in a task of its own — a
service that never returns does not hold the boot. The directory is created at
the boot when it is missing.

```
microos> mkdir /etc/init.d
microos> write /etc/init.d/clock date
microos> reboot
```

The script above runs `date` at every boot. A command that fails is reported in
the log (`dmesg`) and does not stop the rest of the script.

### The configuration

The files of `/etc` are the configuration of the board: the name of a file is
the key, its content is the value. The NVS keeps a copy, so the settings survive
not only a restart but also `mkfs -y`: at the boot the init process writes back
every key that `/etc` lost and saves the files it finds. When the two disagree
the file wins — a person edits `/etc`, the store follows.

```
microos> write /etc/setting hello
microos> config sync
config: 0 file(s) restored, 1 saved
microos> config
setting=hello
```

`config` lists the store, `config sync` restores the missing files and saves
the files of `/etc` in one go.

### The shell

The init process starts the shell, which speaks a small subset of the Linux
commands:

| Command | Meaning |
|---|---|
| `help`, `uname`, `uptime`, `ps`, `dmesg`, `free`, `abi` | the kernel: commands, version, time, tasks, log, memory, syscall table |
| `ls`, `cat`, `write`, `mkdir`, `rm`, `cd`, `pwd`, `df`, `mount`, `mkfs` | the filesystem of the board |
| `modules`, `memtest`, `i2c` | the doctor, the memory test, the I2C scan |
| `date`, `battery`, `beep`, `led`, `input`, `display` | the drivers: clock, charge, buzzer, LED, buttons, panel |
| `config` | the configuration of `/etc` and its copy in the NVS |
| `wifi`, `telnet` | the network: the station and the shell over the air |
| `mfetch`, `mfetch color` | the report about the system with the logo (like fastfetch) |
| `echo`, `sleep`, `clear`, `reboot` | the usual suspects |

The command `mfetch` is the analog of fastfetch: the logo of the system on
the left and a short report about the board on the right — the version and
the build, the hardware, the memory with the blocks that live in it, the
volume, the uptime with the tasks, the modules and the shell. The logo is
the letter M in the characters of the 5x7 font of the screen: one drawing
serves the serial console and the display of the board, and the report
itself is plain text everywhere. `mfetch color` paints the logo.

```
microos> mfetch
M   M  OS           MicroOS 0.2.0-pre
MM MM  Build        Oct 26 2026 17:40:12
M M M  Board        M5Stack StickC Plus2
M   M  SoC          ESP32-PICO-V3-02
M   M  Arch         xtensa-esp32
       Display      1.14" TFT 135x240 (ST7789v2)
       Heap         112/272 KB free
       Psram        1917 KB free
       Allocs       0 block(s), 0 KB
       Volume       5828 of 5832 KB
       Uptime       12 s, 4 task(s)
       Modules      12, 0 problem(s)
       Shell        34 command(s)
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
not in the repository, it is a product of the build: `make bundle` collects
it once the firmware is built, and the ready images go to the releases.
Download the folder, connect the board and run `./flash.sh` — no sources, no
ESP-IDF and no toolchain needed.

## Checks

The whole list of checks runs locally, before a push:

```bash
make ci          # build, tests, sanitizers, smoke test, integrity
```

It builds the host code, runs the unit tests and the same tests under the
sanitizers, the smoke test of the simulator and the integrity of the
repository: the headers of the images, the partition table against
`partitions.csv`, the git objects, the hygiene of the sources and their
formatting. The firmware is not part of `make ci`: it needs ESP-IDF, so it is
built separately with `make idf-build`.

The same list runs in the cloud: every push to `mainline` and
`release_candidate` and every pull request to `mainline` runs the tests,
the same tests under valgrind, the sanitizers, the smoke test and the
integrity of the repository, and builds the firmware for the ESP32 — the
firmware is an artifact of the run. A fresh checkout has no `dist/`, so
the job that built the firmware runs the integrity check again with the
bundle: the sums, the headers of the images, the partition table.

A tag `v*` makes a release: the tests, the build, the integrity check
against the version of the tag (`--expect-version`), and a GitHub release
with the bundle — the binaries, their sums and `flash.sh` — and the notes
about the commits. The tag with a dash (`v0.2.0-pre`) is a pre-release.

## Repository layout

```
Micro_OS/
├── components/
│   ├── kernel/         portable kernel: boot, log, panic, tasks, sched, vfs, kmm, ksys
│   ├── board/          hardware description
│   ├── drivers/        display, input, buzzer, LED, IMU, RTC, battery, mic
│   ├── kshell/         the command interpreter and the built-in commands
│   ├── init/           the init process: the scripts of /etc/init.d
│   ├── config/         the configuration of /etc, persisted in the NVS
│   ├── mfs/            MicroFS on flash and its VFS adapter
│   ├── procfs/         the virtual /proc
│   ├── devfs/          the virtual /dev
│   ├── graphics/       the console on the TFT
│   ├── font/           the 5x7 font of the screen
│   ├── telnet/         the shell over the air on port 23
│   ├── libc/           a libc subset over the syscall ABI
│   ├── wifi/           the WiFi station: the portable core and the radio
│   └── port_esp32/     ESP32 port
├── ports/port_host/    Linux port + simulator
├── main/               firmware entry point (app_main)
├── tests/              unit tests (own minimal framework, no dependencies)
├── tools/              console, bundle, the smoke test, the integrity check
├── docs/               architecture, roadmap, hardware notes, ADRs
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
* **`fork()` is a placeholder.** There is no MMU, so the address space of the
  parent cannot be copied. `fork()` starts a new task that *shares* the memory
  of the parent (like Linux's `CLONE_VM`) and *re-runs the program of the
  parent from its start* — the work done before the call of `fork()` runs
  again in the child. It answers with the number of the child in the parent
  and 0 in the child, so a program keeps the usual shape of POSIX, but a child
  is not a copy of the parent at the point of the call. A real `fork()` (an
  address space of its own, copy on write) is planned for right after the
  first release.
* **One user, no file owners or mode bits.** A file's protection is the
  access its descriptor was opened with - read, write, or both - enforced by
  the kernel with `EBADF`. Owners and `chmod` wait for the first release:
  there is a single user on the board today.
* **8 MB flash / 2 MB PSRAM.** There is one application and no room for an
  update over the air: the kernel takes 2 MB (`factory`) and the rest of the
  flash (5.94 MB) belongs to MicroFS. The whole layout is in
  [partitions.csv](partitions.csv).
* **The ESP32-PICO-V3-02 has no PMIC.** Power and sleep are handled by the ESP32
  itself: `HOLD` (G4) must be driven high after a wake-up or the board powers off.
* **The battery percent is a curve, not a fuel gauge.** Nobody counts the
  milliamp-hours: the charge is looked up from the voltage of the cell, which
  is the truth for a resting cell and an approximation under load. While the
  USB cable is connected the divider sees the charging voltage, so the percent
  sits at the top of the curve (100%) — there the honest number is the
  voltage in `modules`, and `battery` says `(the charger is on)` out loud.
* **One power button plus two user buttons** (A, B), and a third one (C) on Plus2.

## License

MIT — see [LICENSE](LICENSE).
