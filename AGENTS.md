# AGENTS.md

Guidance for coding agents working in this repository. The human-facing
documentation is `README.md`, `CONTRIBUTING.md` and `docs/`; this file is the
short version of what an agent needs before touching code.

## What this is

Micro OS: a Linux-like kernel for the M5Stack StickC Plus2 (ESP32-PICO-V3-02,
240 MHz, 2 MB PSRAM, 8 MB flash, 135x240 TFT). Scheduler, syscall ABI, VFS,
`/proc`, `/dev`, MicroFS, drivers, shell — all on bare metal, no MMU.

Two builds from one tree:

* **host** — plain CMake + GCC on Linux: unit tests and a simulator. The main
  development loop; no hardware and no ESP-IDF involved.
* **ESP32** — the same sources as an ESP-IDF v5.3 project.

Current phase: 0–4 done, phase 5 partly done (WiFi station, telnet). Details in
`docs/roadmap.md`.

## The rule that matters most

**The kernel knows nothing about hardware.** `components/kernel/` is portable
C11 with zero ESP-IDF dependencies and talks to the platform only through
`kernel/port.h`. Everything hardware-specific lives in `ports/port_host/` or
`components/port_esp32/` (and in drivers, which the host build can stub).
If a change puts `esp_*`, FreeRTOS or a pin number into `components/kernel/`,
the change is wrong.

Corollaries:

* Never call libc `printf`/`malloc` from kernel code — use `klog`, `kmm_alloc`.
* A kernel test that needs real hardware means platform and portable code got
  mixed.

## Layout

| Path | Contents |
|---|---|
| `components/kernel/` | boot levels, log, panic, version, tasks, sched, kmm (allocator + slabs), vfs, kmod, ksys (syscall table/ABI) |
| `components/board/` | board description (SoC, flash, PSRAM, pins) |
| `components/drivers/` | display (ST7789v2), input, buzzer, led, imu, rtc, battery, mic + `Kconfig` |
| `components/kshell/` | command interpreter and the built-in commands (`kshell_files.c`, `kshell_memtest.c`, `kshell_mfetch.c`) |
| `components/mfs/` | MicroFS on flash (DATA/META/INFO sections) + VFS adapter `mfs_vfs.c` |
| `components/procfs/`, `components/devfs/` | virtual filesystems |
| `components/graphics/`, `components/font/` | console on the TFT, 5x7 font, framebuffer drawing |
| `components/telnet/` | shell over TCP port 23 (protocol part is host-testable) |
| `components/libc/` | libc subset over the syscall ABI |
| `components/wifi/` | portable station core + ESP32 radio |
| `components/port_esp32/` | ESP32 implementation of `kernel/port.h` |
| `ports/port_host/` | Linux port, simulator, host PSRAM pool |
| `main/` | firmware entry point (`app_main`, init, self test, hardware doctor) |
| `tests/` | unit tests, own framework (`framework/ktest.h`), corpus `tests/corpus/` |
| `tools/` | `console.py`, `device-test.sh`, `sim_smoke.py`, `integrity_check.py`, `bundle.sh` |
| `docs/` | architecture, roadmap, memory layout, syscall ABI, phases, ADRs, hardware notes |

Useful headers to read first: `kernel/port.h`, `kernel/boot.h`,
`kernel/ksys.h`, `kernel/vfs.h`, `kernel/kmm.h`, `kernel/kmod.h`.

## Commands

The full reference is in `commands.md` (`commands_ru.md`); the short version:

```bash
make test                      # build + run unit tests (host)
make sim                       # run the kernel simulator
make smoke                     # boot the simulator and check its answers
make integrity                 # repo checks: bundle, images, partitions, git, hygiene, format
make ci                        # build, test, test-asan, smoke, integrity
make format                    # clang-format the C sources
make idf-build                 # ESP32 firmware (needs ESP-IDF exported)
make idf-flash / idf-monitor / idf-size
make bundle                    # dist/ with firmware + flash.sh
```

Direct host build (what `make` does):

```bash
cmake -B build-host -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Device build on this machine (ESP-IDF is a **sibling** of the repo):

```bash
. ../esp-idf/export.sh
idf.py set-target esp32        # once
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`make idf-build` finds ESP-IDF on its own: `$IDF_PATH`, then `../esp-idf`
(sibling of the repo), then `~/esp/esp-idf`. Point it elsewhere with
`IDF_EXPORT=/path/to/export.sh`. It fails with a clear message when no ESP-IDF
is found.

Requirements: `cmake`, `ninja` (make targets use `-G Ninja`), `gcc`, `python3`;
ESP-IDF brings its own toolchain and Python env. On this machine ESP-IDF v5.3
lives in `/home/nik/Projects/opencode/esp-idf` and its env is
`~/.espressif/python_env/idf5.3_py3.14_env`. The device build writes
`build/compile_commands.json`; the repo root has a `compile_commands.json`
symlink to it so clangd (and Serena) resolve symbols with the real target flags.

## Code conventions

* C11. No C++, no GNU extensions in the portable kernel.
* 4 spaces, max 100 columns, no tabs, LF, trailing newline (enforced by
  `make integrity`).
* `-Wall -Wextra -Werror`: warnings are errors.
* Prefixes: `k_` kernel, `board_` board, `drv_` driver, `port_` platform,
  `mfs_` filesystem, `ksys_`/`KSYS_` syscalls, `klog` logging.
* Globals: `g_` prefix plus a unit suffix (`_s`, `_ms`, `_kb`).
* Include guards: `MICRO_OS_<PATH>_H`.
* Style is in `.clang-format` (LLVM base, Linux braces); do not reformat by
  hand, run `make format`.
* Comments explain *why*, in whole sentences; the codebase reads like prose.
* English is canonical; Russian translations are `*_ru.md` and may lag.

## Tests

* Framework: `tests/framework/ktest.h` — `KT_CHECK`, `KT_CHECK_INT_EQ`,
  `KT_CHECK_STR_EQ*`, no external dependencies.
* Adding a test case needs four touches:
  1. `tests/cases/test_<thing>.c`,
  2. the `void test_<thing>(void);` declaration in `tests/include/tests/tests.h`,
  3. the call in `run_tests()` in `tests/src/main.c`,
  4. the file in `add_executable(microos_tests ...)` in `tests/CMakeLists.txt`.
* Tests run inside a real kernel task, so they exercise the task API.
* `tests/corpus/vectors.txt` holds the fixed numbers (including negatives and
  boundary values) shared by the tests; new numeric behaviour should reuse it.
* Kernel behaviour without a test is unfinished work.
* Sanitizers: `make test-asan`.

## Syscalls and ABI

`components/kernel/include/kernel/ksys.h` is a frozen contract: numbers are
never reused or reordered, new calls take the next free number. The ABI is
documented in `docs/syscall-abi.md` and covered by `tests/cases/test_ksys*.c`.
Changing it is an architectural change (see below).

## Docs and decisions

* `docs/roadmap.md` — phases and open items; the place to look for "what next".
* `docs/architecture.md` — design, boot levels, locking model, resource budget.
* `docs/phases/phase<N>.md` — what a phase delivered, which bugs it cost.
* `docs/adr/` — architecture decision records; a significant change needs one.
* `docs/hardware/stickc-plus2.md` — pinout and board quirks; read before
  writing a driver.
* `docs/memory.md`, `docs/libc.md`, `docs/syscall-abi.md` — contracts worth
  not breaking.

## Commits

Linux style, one logical change per commit, area prefix, first line <= 72 chars
without a trailing dot, body explains *why*:

```
kernel: add task priority inheritance
vfs: fix path lookup for trailing slash
drivers: fix i2c timeout on mpu6886
```

Branches: `mainline` is always green, `release_candidate` feeds releases,
features go to `feature/<what>`, fixes to `fix/<what>`. Tags `vX.Y.Z`
publish a release; `vX.Y.Z-pre` is a pre-release and must match the kernel
version.

## Locking and concurrency (easy to get wrong)

* The panel SPI is one hardware resource: every drawing/flush/offset/rotation
  call takes the recursive display lock.
* The filesystem is one state: every public `mfs_*` call takes a recursive
  "sleep lock" (no blocking primitive yet). Open file position/flags live in
  the descriptor, not in the FS.
* The allocator is behind one lock (`kmm`), the host PSRAM pool behind a mutex.
* Error-code translation happens only in `components/mfs/src/mfs_vfs.c`;
  filesystem codes and syscall codes are not identical.

## Hardware and resource limits

* No MMU: no memory protection, `fork()` temporarily shares the parent memory
  and re-runs the parent program (see README "Known limitations").
* Flash: 8 MB — `factory` 2 MB + MicroFS 5.75 MB, no OTA slot
  (`partitions.csv`, checked by `make integrity`).
* Internal SRAM ~100 KB is scarce: big buffers go to PSRAM, kernel/drivers get
  internal RAM, 48 KB is reserved from applications.
* Board quirks: no PMIC (`HOLD` G4 must be high after wake), RTC needs a backup
  battery, LED and IR share G19, one power + two user buttons.

## Repo hygiene

* Generated and must not be committed: `build*/`, `sdkconfig`, `sdkconfig.old`,
  `dist/`, `compile_commands.json`, `.serena/cache`, `esp-idf/`.
* `.serena/project.yml` and `opencode.jsonc` are project-local tooling config
  (Serena MCP with clangd for symbol navigation); they are safe to keep.
* `make integrity` also checks git objects, UTF-8/LF, trailing whitespace,
  tabs, line length and clang-format — run it before claiming a task is done.

## Where to go next (snapshot)

Open items from `docs/roadmap.md`:

* Phase 5: init starting daemons from `/etc/init.d`, configuration from `/etc`
  persisted in NVS, HTTP client + NTP on top of the RTC, demo applications
  (sensor logger, IR remote, clock, weather station).
* After the first release: RTC alarm + deep-sleep wake, `execve` and a program
  format, per-file mode bits/owners in MicroFS, `drv_ir`, microphone driver,
  MQTT client, factory reset.
