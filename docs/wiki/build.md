# Build and run

Three builds come out of one tree, and each of them is a check of the
portability as much as a product:

| Build | What it produces | Commands | Directory |
|---|---|---|---|
| host | the unit tests and the simulator, as a Linux process | `make build`, `make test`, `make sim`, `make smoke` | `build-host/`, `build-asan/`, `build-tsan/` |
| x86-64 | a bare-metal kernel and its loader, for QEMU | `make x86-build`, `make x86-run`, `make x86-smoke` | `build-x86/` |
| ESP32 | the firmware of the board, through ESP-IDF v5.3 | `make idf-build`, `make idf-flash`, `make bundle` | `build/` |

The null port (`ports/port_null`) has no build of its own: it is linked into
the host build as `microos_null_probe`, and the link is the check.

## Dependencies

| Package | Needed by | Arch / CachyOS | Debian / Ubuntu |
|---|---|---|---|
| `cmake`, `ninja` | every build | `cmake ninja` | `cmake ninja-build` |
| `gcc` | the host and x86 builds | `gcc` | `gcc` |
| `python3` | the tools, the checks, ESP-IDF | `python` | `python3` |
| `qemu-system-x86_64` | running the x86-64 machine | `qemu-system-x86` | `qemu-system-x86` |
| GTK backend of QEMU | the window of `make x86-run` | `qemu-ui-gtk` | `qemu-system-gui` |
| `clang-format` | `make format`, the format check | `clang-format` | `clang-format` |
| `valgrind` | the cloud run of the tests, optional | `valgrind` | `valgrind` |
| `pyserial` | `tools/console.py`, `tools/stream_console.py` | `python3 -m pip install pyserial` | the same |

The packages of the last two columns are installed with `sudo pacman -S
--needed ...` or `sudo apt install ...`. ESP-IDF is needed only for the board;
it brings its own toolchain and Python environment. [`commands.md`](../../commands.md)
is the reference for the packages and for the variables that override the
defaults.

## The host build

```bash
make build       # cmake -B build-host -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
make test        # build, then ctest: the unit tests and the null probe
make test-asan   # the same tests with the address and UB sanitizers
make test-tsan   # the same tests under the race detector
make sim         # build, then run build-host/ports/port_host/microos_sim
make smoke       # boot the simulator and check what it says
```

The same by hand:

```bash
cmake -B build-host -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
build-host/ports/port_host/microos_sim
```

The simulator is the whole system on a volume in a file: MicroFS, `/proc`,
`/dev`, the configuration of `/etc`, the init scripts and the shell. The
environment shapes it:

| Variable | Meaning | Default |
|---|---|---|
| `MICROOS_VOLUME` | the file of the MicroFS volume | `/tmp/microos-sim.img` |
| `MICROOS_KV_DIR` | the directory of the key-value store | `/tmp/microos-kv` |
| `MICROOS_SCREEN` | draw the console into this file instead of nowhere | unset |
| `MICROOS_INPUT` | a script of button events for `input` | unset |

`make sim` is interactive; `make smoke` (`tools/sim_smoke.py`) drives it with
a list of commands and checks the transcript. `make clean` removes
`build-host/` and `build-asan/` only: `build-tsan/` and `build/` stay.

## The x86-64 build

```bash
make x86-build            # the loader, the kernel and the programs, into build-x86/
make x86-all              # the complete machine: network, disk and run.sh
make x86-all-run          # boot it in a window (x86-all-run-serial: a terminal)
make x86-disk             # build-x86/programs.img: /bin and /etc of MicroFS
make x86-run              # a QEMU window: framebuffer 1280x800 and the keyboard
make x86-run-serial       # the same machine in a terminal (-nographic)
make x86-smoke            # boot and check the transcript, no window
make x86-vga-smoke        # check both screens and the keyboard, without a display
make x86-run-disk         # the window with a disk of its own, kept between runs
make x86-run-disk-serial  # the same disk in a terminal
make x86-disk-smoke       # two boots on one image: a file survives both
```

The build is a CMake project of its own (`ports/port_x86_64`), because the
kernel there has no libc under it. The console of the screen draws with the
8x16 font of the PC - Spleen 8x16 2.3.0 (BSD-2-Clause), generated into
`ports/port_x86_64/src/font_spleen.c` from the BDF in `third_party/spleen` by
`tools/spleen_font.py`; it is part of the kernel image, so `tools/bundle.sh`
puts `LICENSE.spleen` into the x86-64 archive (`docs/licensing.md`). The
machine boots from three files:
`build-x86/loader32.elf` (a multiboot loader that QEMU takes with `-kernel`),
`build-x86/microos64.bin` (the flat kernel image) and
`build-x86/programs.img` (the disk). The programs of `/bin` are built first
(`hello.mox`, `crash.mox`, `fork.mox`, `badptr.mox`, ...) and written into a
MicroFS volume of the disk by `tools/mkfs_mfs.c`; the kernel carries no copy
of a program, and the boot mounts the volume and reads `/bin/hello` with
`execve`.

`make x86-all` is the machine a person can start without knowing which of
the builds it is made of: `build-x86-all/` holds `loader32.elf`,
`microos64.bin`, `x86-all.img` (the disk with `/bin` and `/etc`) and a `run.sh`
that boots the three with a virtio-net card on the user network of QEMU (port
8080 forwarded). `X86_SERIAL=1 ./build-x86-all/run.sh` gives the serial
console and no window. The target is not `make bundle`; the release artifact
is still the one of `tools/bundle.sh`.

The display backend is a package of its own in some distributions: the target
asks QEMU for its list of backends first and prints the command to run when
the one asked for is missing. `make x86-run X86_DISPLAY=sdl` picks SDL, and
`make x86-run-serial` is the machine for a box without a display server at
all. The window shows the framebuffer of 1280x800 of ADR 0023 (`-vga std`);
a machine whose card has no Bochs VBE interface keeps the text mode of 80x25,
which is what `make x86-run` shows with `-vga cirrus`.

The disk is a raw image of 8 MB, made by `make x86-disk`
(`tools/mkfs_mfs.c`, which links `components/mfs`) and kept in
`build-x86/programs.img`; `make x86-run-disk` copies it once to
`build-x86/disk.img` and keeps that copy between runs. Both are generated,
not committed; `make clean` does not remove `build-x86/`, so
`rm -rf build-x86` is the way to start over. The details are in the port's
README, [`docs/ports/x86_64/README.md`](../ports/x86_64/README.md).

## The ESP32 build

`make idf-build` finds ESP-IDF on its own, in this order: `$IDF_PATH`, a
checkout beside the repository (`../esp-idf`), then `~/esp/esp-idf`. Point it
elsewhere with `IDF_EXPORT=/path/to/export.sh`; there is no need to source
`export.sh` for the `make idf-*` targets.

```bash
make idf-build     # idf.py -D IDF_TARGET=esp32 build
make idf-flash     # flash over USB; make idf-flash IDF_PORT=/dev/ttyACM0
make idf-monitor   # the serial monitor (leave with Ctrl+])
make idf-size      # the size report of the firmware
make bundle        # build everything and collect dist/ for distribution
```

A first checkout needs the target once. With ESP-IDF exported:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py menuconfig      # the Micro OS Kconfig (components/drivers/Kconfig)
idf.py erase-flash     # wipes everything, MicroFS data included
```

The board is on `/dev/ttyACM0` and the port belongs to the group `uucp`:

```bash
sudo gpasswd -a $USER uucp    # log out and back in afterwards
```

`tools/device-test.sh` builds, flashes and reads the console, and prints
`RESULT: PASS` when the self test, the hardware doctor and the shell are up.
The layout of the flash is in [`partitions.csv`](../../partitions.csv):
two application slots of 1.125 MiB, 132 KiB of programs in `progs` and 5.56 MiB
of MicroFS (ADR 0022). The board's
own README is [`docs/ports/esp32/README.md`](../ports/esp32/README.md), the
pins and quirks are in
[`docs/ports/esp32/hardware.md`](../ports/esp32/hardware.md).

## The checks around the builds

```bash
make integrity   # the bundle, the image headers, the partition table, git, hygiene, format
make format      # reformat the C sources with clang-format
make ci          # build, test, test-asan, test-tsan, smoke, [x86-smoke], integrity
```

`make ci` runs the x86-64 smoke test when `qemu-system-x86_64` is on `PATH`
and says so when it is not. It does not build the firmware, and it does not run
the VGA or disk smoke tests or the device test: those are separate commands.
[`testing.md`](testing.md) explains what each check proves.

## When the build stops

[`troubleshooting.md`](troubleshooting.md) is the table of the errors that
happen most: ESP-IDF not found, a toolchain that the registry of ESP-IDF does
not know, a QEMU without a display, `/dev/ttyACM0` that belongs to somebody
else, and the link errors of a stale build directory.

[Back to the wiki index](README.md)
