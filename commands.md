# Micro OS — commands

Every command is run from the root of the repository. There are two builds from
the same sources: the **host** build (unit tests and the simulator, no hardware)
and the **device** build (ESP32 firmware through ESP-IDF v5.3).

> Russian version: [commands_ru.md](commands_ru.md)

## 1. Prerequisites

| Need | Why | Install (Arch/CachyOS) |
|---|---|---|
| `cmake`, `ninja` | the host build and ESP-IDF | `sudo pacman -S --needed cmake ninja` |
| `gcc` | the host build | `sudo pacman -S --needed gcc` |
| `python3` | the tools in `tools/` and the ESP-IDF env | `sudo pacman -S --needed python` |
| `clang-format` | `make format` and the format check | `sudo pacman -S --needed clang-format` |
| `valgrind` | the CI run of the tests (optional locally) | `sudo pacman -S --needed valgrind` |
| `pyserial` | `tools/console.py`, `tools/stream_console.py` | `python3 -m pip install pyserial` (or use the ESP-IDF env) |

ESP-IDF v5.3 is needed only for the device build. The tools look for it in this
order: `$IDF_PATH`, a checkout beside the repository (`../esp-idf`), then
`~/esp/esp-idf`. Override with `IDF_EXPORT=/path/to/export.sh`.

First-time ESP-IDF setup:

```bash
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
. ~/esp/esp-idf/export.sh
```

The serial port belongs to the `uucp` group on most distributions:

```bash
sudo gpasswd -a $USER uucp    # log out and back in afterwards
```

## 2. Host build, tests and simulator

| Command | What it does |
|---|---|
| `make` | the same as `make build` |
| `make build` | configure and build `build-host/` with Ninja |
| `make test` | build and run the unit tests through `ctest` |
| `make test-asan` | the same tests with the address and UB sanitizers, in `build-asan/` |
| `make sim` | build and run the kernel simulator `build-host/ports/port_host/microos_sim` |
| `make smoke` | boot the simulator and check the shell and the subsystems (needs `make build` first) |
| `make integrity` | the integrity check: bundle, image headers, partition table, git, sources, formatting |
| `make format` | reformat the C sources with `clang-format` |
| `make clean` | remove `build-host/` and `build-asan/` (the device `build/` stays) |
| `make ci` | `build`, `test`, `test-asan`, `smoke`, `integrity` — run it before pushing |

The same build by hand:

```bash
cmake -B build-host -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
build-host/ports/port_host/microos_sim
```

Useful tool options:

```bash
python3 tools/sim_smoke.py --verbose
python3 tools/integrity_check.py --verbose
python3 tools/integrity_check.py --skip-format --skip-git
python3 tools/integrity_check.py --expect-version 0.2.0
```

## 3. Device build and flashing

| Command | What it does |
|---|---|
| `make idf-build` | build the ESP32 firmware into `build/` |
| `make idf-flash` | flash the firmware over USB |
| `make idf-monitor` | attach the serial monitor (leave with `Ctrl+]`) |
| `make idf-size` | the firmware size report |
| `make bundle` | build and collect the flashable bundle into `dist/` |

The Makefile passes `IDF_TARGET=esp32` (from `sdkconfig.defaults`). The port is
auto-detected by `idf.py`; give it explicitly when there are several boards:

```bash
make idf-flash IDF_PORT=/dev/ttyACM0
```

Without the Makefile (after `. ../esp-idf/export.sh`):

```bash
idf.py set-target esp32      # once for a fresh checkout
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py size
idf.py menuconfig            # the Micro OS Kconfig (components/drivers/Kconfig)
idf.py erase-flash           # wipes everything, including the MicroFS data
```

## 4. Running on the board

```bash
tools/device-test.sh                  # build, flash and read the console
tools/device-test.sh /dev/ttyACM0     # with an explicit port
CONSOLE_SECONDS=60 tools/device-test.sh /dev/ttyACM0
```

The script exits with `RESULT: PASS` when the self test, the hardware doctor and
the shell came up.

Read the console without flashing:

```bash
tools/console.py                          # auto-detect the port, 30 seconds
tools/console.py /dev/ttyACM0 60          # a port and a duration
tools/console.py /dev/ttyACM0 20 --reset --send "modules"
tools/stream_console.py                   # interactive console, Ctrl+Q to leave
```

### Daemons at boot

Every file of `/etc/init.d` is a script of the init process: one shell command
per line, `#` starts a comment, and each script runs in a task of its own. The
directory is created at the boot when it is missing.

```bash
# from the shell of the board:
mkdir /etc/init.d
write /etc/init.d/clock date
reboot
```

## 5. Overrides

| Variable / argument | Default | Meaning |
|---|---|---|
| `IDF_EXPORT` | auto-detected | path to `export.sh` of another ESP-IDF |
| `IDF_TARGET` | `esp32` | the chip of the build |
| `IDF_BUILD` | `build` | the directory of the device build |
| `BUILD_DIR` | `build-host` | the directory of the host build |
| `ASAN_DIR` | `build-asan` | the directory of the sanitizer build |
| `CONSOLE_SECONDS` | `30` | how long `device-test.sh` reads the console |
| `IDF_PORT` | auto | the serial port for `make idf-flash`/`idf-monitor` |

## 6. Releases

```bash
make ci                                        # the local list of checks
python3 tools/integrity_check.py --expect-version 0.2.0
git tag v0.2.0-pre && git push origin v0.2.0-pre
```

A tag `vX.Y.Z` runs the tests and the firmware build in GitHub Actions and
publishes a release with `dist/` (the binaries, their sums, `flash.sh`). A tag
with a dash (`vX.Y.Z-pre`) is a pre-release. The version of the kernel is
`KERNEL_VERSION_*` in `components/kernel/include/kernel/config.h` and must match
the tag.

## 7. Two notes

* `make ci` does not build the firmware: that needs ESP-IDF and is a separate
  `make idf-build` (the cloud does both).
* The device build writes `build/compile_commands.json`; the root
  `compile_commands.json` is a symlink to it, so clangd and the Serena MCP
  resolve symbols with the real target flags.
