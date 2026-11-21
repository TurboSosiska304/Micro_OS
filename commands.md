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
| `make smoke` | boot the simulator and check the shell, the filesystem and the network (needs `make build` first) |
| `make coverage` | the host build with `--coverage`, the tests, the simulator and the gcov report of `components/kernel` (70% of lines) |
| `make reproducible` | build the disk image twice and compare the bytes (`SOURCE_DATE_EPOCH`) |
| `make integrity` | the integrity check: bundle, image headers, partition table, git, sources, formatting |
| `make format` | reformat the C sources with `clang-format` |
| `make clean` | remove `build-host/`, `build-asan/`, `build-coverage/` (the device `build/` stays) |
| `make ci` | `build`, `test`, `test-asan`, `test-tsan`, `smoke`, `coverage`, `x86-smoke` when QEMU is there, `reproducible`, `integrity` — run it before pushing |

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

### Configuration

The files of `/etc` are the configuration; the NVS keeps the copy that
survives a restart and `mkfs -y`. The boot restores the missing files and
saves the ones it finds; the file wins when the two disagree.

```bash
# from the shell of the board:
write /etc/setting hello
config sync        # restore the missing files, save the files of /etc
config             # list the store
```

### The mode, the owner, a program and the path

A file has a mode and an owner, and the shell changes them through the system
calls (numbers 23 and 24), not through the filesystem. `stat` shows what a
record really holds, and `exec` asks the same ABI for a program: the file is
read by the kernel, checked against the format `MOX1` and handed to the port.

| Command | What it does |
|---|---|
| `fsck` | check the volume: the first problem, sizes, free blocks; `fsck fat ... -y` repairs the FAT volume |
| `chmod <mode> <path>` | the mode in octal: `0644`, `755`, `0700` (the nine bits `rwx`) |
| `chown <uid> <path>` | the owner, a decimal uid `0..32767` |
| `stat <path>` | the type, the mode, the owner and the size of one object |
| `path` | the directories of `PATH` |
| `path <dirs>` | set `PATH` with `:` between directories; `path ""` is the default |
| `exec <program> [arguments...]` | run a program, by a path or found in `PATH` |

```bash
# from the shell of the board:
chmod 0700 /etc/init.d
chown 0 /etc/setting
chmod 0755 /bin/hello
stat /bin/hello
# -rwxr-xr-x 0 1234 /bin/hello
# type: file
# mode: 0755
# owner: 0
# size: 1234
exec hello one two          # found in PATH; the board cannot run a program
```

`stat` prints the line of `ls -l` first and then the same numbers as fields,
so a `chmod`/`chown` is checked without a calculator. An object of a
filesystem that keeps no modes (procfs, devfs, a volume written by an older
release) prints `mode: (not set)` and no `ls` line: nine dashes would read as
`0000`, which is a statement the record does not make.

`exec` looks at the name before it runs anything. A name with a slash is a
path and is used as it is; a name without one is looked up in `PATH`, where
the first regular file with an execute bit wins. A search that found nothing
says where it looked: `exec: hello: not found in /bin:/usr/bin`. `path` keeps
that value in the shell and not in `/etc` - it starts at `/bin:/usr/bin`, and
the splitter of the shell has no quotes, so the two characters `""` are how a
person types the empty argument that brings the default back.

A mode that is not octal (`0999`), a uid above `32767` and a missing path are
refused with a sentence: `not an octal mode`, `a uid is 0..32767`, `no such
file`. A `chmod 000` is not a way to lock a file either: the filesystem keeps a
zero as "the record predates the modes", so it refuses the call instead of
storing a number it would read back as `0644`.

`exec` returns to the prompt only when no program ran, and says why: `ENOSYS`
becomes "this machine cannot run a program", `EPERM` "the file has no execute
bit", `EINVAL` "not a MOX1 image of this system". The board and the simulator
have no window of programs; the x86-64 port is the machine that runs one.

### Flags and the help

Every command answers `-h` and `--help` with its usage line, the one line of the
list of `help` and the notes about its flags; the command does not do its work,
and the status is 0, because the help that was asked for was given. A command
whose words are its own (`write`, `exec`) only answers a help request that
stands before them: `write /etc/setting -h` writes the two characters.

```bash
# from the shell of the board:
help                     # the list of the commands
help ls                  # the usage and the notes of one command
ls -h                    # the same text
chmod --help             # ... for a command that parses no flags
help nosuch              # help: nosuch: no such command
```

The flags are read by the shell itself, not by a `getopt` of libc: `-la` and
`-l -a` are the same, an option the command does not have is refused with its
usage line (`ls: unknown option: -z`), and `--` ends the options, so a name
that begins with a dash can still be given:

```bash
# from the shell of the board:
cat -- -notes            # a file whose name is "-notes"
rm -- -notes
```

| Command | Flag | Meaning |
|---|---|---|
| `ls` | `-l` | the mode, the owner, the size and the date of every entry |
| `ls` | `-a` | `.` and `..` first |
| `tree` | `-d` | the directories only |
| `tree` | `-a` | accepted for hidden names; this system has none yet |
| `cat` | `-n` | the number of the line in front of every line |
| `rm` | `-r` | a directory with everything in it; without it a directory is refused |
| `df` | `-h` | the sizes in KB and MB instead of blocks |

`df` is the one command where `-h` is not the help but a flag of its own, as in
`df` of Linux: `df --help` prints its text. `ls -l` writes `?` where an object
keeps no mode (devfs, a volume of an older release): nine dashes would read as
"nobody may touch it", which the record does not say.

```bash
# from the shell of the board:
ls -l /etc               # drwxr-xr-x 0 0 init.d
ls -a /                 # . and .. beside the real entries
df -h                    # /: 487 KB total, 482 KB free
cat -n /etc/setting      # "     1" and the line of the file, a tab between
rm -r /tmp/mine          # the directory and everything in it
```

### HTTP and the clock

```bash
# from the shell of the board:
http get http://10.10.10.1/   # the status and the body of the answer
ntp                           # pool.ntp.org, the time goes into the RTC (UTC)
ntp time.google.com           # another server
```

## 5. Overrides

| Variable / argument | Default | Meaning |
|---|---|---|
| `IDF_EXPORT` | auto-detected | path to `export.sh` of another ESP-IDF |
| `IDF_TARGET` | `esp32` | the chip of the build |
| `IDF_BUILD` | `build` | the directory of the device build |
| `BUILD_DIR` | `build-host` | the directory of the host build |
| `ASAN_DIR` | `build-asan` | the directory of the sanitizer build |
| `COVERAGE_DIR` | `build-coverage` | the directory of the coverage build |
| `COVERAGE_THRESHOLD` | `70` | the least percentage of kernel lines the report accepts |
| `SOURCE_DATE_EPOCH` | `0` | the moment of the records and the build id of a reproducible build |
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
