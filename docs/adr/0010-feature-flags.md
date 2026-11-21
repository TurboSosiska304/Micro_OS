# ADR 0010 - Features behind compile-time flags

* Status: accepted, implemented
* Date: 2026-11-21

## Context

The same sources build for four machines, and they do not need the same
things. The x86-64 port has no PSRAM and no network, yet `kmm` is compiled with
the PSRAM pool and the `free` command prints a PSRAM line that can only be
zero. The ESP-IDF image always carries MQTT, the HTTP page and the NTP client,
even when the board has no broker and the person wants the flash for something
else. MicroFS is linked into every image, and the second filesystem is about to
be added.

A board with 8 MB of flash and ~100 KB of internal SRAM cannot pay for every
feature on every build. A host build, on the other hand, wants all of them:
the tests are where the code is exercised.

The kernel stays hardware-independent: a flag says which code is compiled, not
which hardware exists. The port contract does not change; a port that has no
PSRAM still answers the two calls with zero, it just does not get them called.

## Decision

### 1. Two kinds of flags

| Prefix | Owner | Examples |
|---|---|---|
| `KERNEL_*` | `components/kernel/include/kernel/config.h` | `KERNEL_PSRAM`, `KERNEL_EXEC` |
| `MICRO_OS_*` | a component's `CMakeLists.txt` / `Kconfig` | `MICRO_OS_NET_MQTT`, `MICRO_OS_FS_FAT32` |

A kernel flag is a compile-time constant of the portable kernel. A component
flag decides whether the sources of that component are compiled at all. Neither
is a runtime setting: the value is fixed when the image is built.

Every flag is `#ifndef`-guarded in the header and passed with
`-DNAME=0|1` (CMake) or `CONFIG_NAME=y` (ESP-IDF). A flag that is not passed
keeps the default of the header, so a port that says nothing builds the
complete kernel.

### 2. The first set

Kernel:

* `KERNEL_PSRAM` - the PSRAM pool of `kmm`, the statistics and the `psram`
  line of `free` (default 1; off on x86-64 and on the null port);
* `KERNEL_EXEC` - the MOX1 parser, `k_exec_run`, the `execve` path and the
  `exec` command (default 1; every build compiles it, and a machine that
  cannot enter a program answers `ENOSYS` from `k_port_exec_supported()` -
  the board entered the group with ADR 0018).

Network (component `net` and the components beside it):

* `MICRO_OS_NET` - the component as a whole;
* `MICRO_OS_NET_HTTP` - the HTTP client;
* `MICRO_OS_NET_HTTP_SERVER` - the page of the state (`httpd`);
* `MICRO_OS_NET_NTP` - the clock;
* `MICRO_OS_NET_MQTT` - the broker client and the `mqtt` command;
* `MICRO_OS_TELNET` - the shell on port 23;
* `MICRO_OS_WIFI` - the station of the board.

Filesystems (the flags of ADR 0009):

* `MICRO_OS_FS_MFS` - MicroFS, the root of every machine (default 1);
* `MICRO_OS_FS_FAT32` - FAT12/16/32, read and written (default 0);
* `MICRO_OS_FS_EXT2` - ext2, read-only, the first reader with inodes
  (default 1 on the host; 0 elsewhere).

Drivers keep their own Kconfig switches (`MICRO_OS_MIC` is the first); the
same rule applies to them.

### 3. The defaults per build

| Flag | host | ESP32 | x86-64 | null |
|---|---|---|---|---|
| `KERNEL_PSRAM` | 1 | 1 | **0** | 0 |
| `KERNEL_EXEC` | 1 | 1 | 1 | 1 |
| `MICRO_OS_NET` | 1 | 1 | 0 | 0 |
| `MICRO_OS_NET_HTTP` | 1 | 1 | 0 | 0 |
| `MICRO_OS_NET_HTTP_SERVER` | 1 | 0 | 0 | 0 |
| `MICRO_OS_NET_NTP` | 1 | 1 | 0 | 0 |
| `MICRO_OS_NET_MQTT` | 1 | **0** | 0 | 0 |
| `MICRO_OS_TELNET` | 1 | 1 | 0 | 0 |
| `MICRO_OS_WIFI` | 1 | 1 | 0 | 0 |
| `MICRO_OS_FS_MFS` | 1 | 1 | 1 | 1 |
| `MICRO_OS_FS_FAT32` | 1 | 0 | 0 | 0 |
| `MICRO_OS_FS_EXT2` | 1 | 0 | 0 | 0 |

The host builds everything because the tests are the place where every path is
compiled. The x86-64 port has one memory, no network and no PSRAM, so it turns
all of that off; its programs and its disk stay. The board keeps the features
it ships today, with MQTT off until a build asks for it and the page of the
state off because it is a command a person starts. A release that wants MQTT
adds `CONFIG_MICRO_OS_NET_MQTT=y` to `sdkconfig.defaults`; a minimal board
turns the rest off the same way.

The ext2 reader is the one filesystem whose host default is ON and whose
sources are compiled by the tests even when `make test-minimal` turns the
flag off: `components/fs_ext2` is linked as a library by `make test`, and
the three files are compiled into the test binary by the minimal
configuration, so no path of the reader is ever left uncompiled (ADR 0019).
On the board and on x86-64 the flag is off, because no image mounts an ext2
volume at the boot; an image that wants one turns it on and registers the
type itself.

### 4. How a disabled feature behaves

* A kernel flag removes the code and the callers use the fallback: with
  `KERNEL_PSRAM=0`, `kmm_alloc` asks the internal heap only and `free` does
  not print a PSRAM line; with `KERNEL_EXEC=0`, `execve` answers `ENOSYS` and
  the `exec` command is not registered.
* A component flag removes the source from the image. The command it
  registered disappears with it; `help` does not list it.
* A port still implements the whole contract, so the linker never wants a
  symbol that the flag removed.

### 5. Checks

* `make test` builds the host with every flag on, so no path is left
  uncompiled.
* `make x86-smoke` and `make x86-vga-smoke`/`make x86-disk-smoke` build the
  minimal x86-64 image.
* `make idf-build` uses the board defaults; `make integrity` checks the image
  budget, and a new check refuses a disabled feature in the map (for example
  `mqtt_` with `MICRO_OS_NET_MQTT=0`).
* A second host configuration with the optional features off is a target of
  its own (`make test-minimal`), not a part of `make ci` at first.

### 6. The implementation

* `KERNEL_PSRAM` and `KERNEL_EXEC` live in `kernel/config.h` behind
  `#ifndef`, with the default of a complete kernel. The host CMake options
  `KERNEL_PSRAM` and `KERNEL_EXEC` turn them into `-DNAME=0|1`; the x86-64
  port names the same options with its own defaults; the ESP-IDF kernel
  component passes `KERNEL_PSRAM=1 KERNEL_EXEC=0` to everything that links it.
* The component flags are CMake options on the host (`MICRO_OS_NET`,
  `MICRO_OS_NET_HTTP`, `_HTTP_SERVER`, `_NTP`, `_MQTT`, `MICRO_OS_TELNET`,
  `MICRO_OS_WIFI`) and `CONFIG_MICRO_OS_*` symbols from `Kconfig` on the
  board. C sources read the numeric `MICRO_OS_*` macros, which the component
  CMake passes alongside the `CONFIG_*` value.
* A source of a disabled feature is not in the `SRCS` list of its component;
  `net.c` registers only the commands of the features that are compiled.
* `make test-minimal` is the host build with the optional features off; it
  builds and runs the tests that do not need them (the exec and net cases are
  guarded in the source). `tools/integrity_check.py` reads the map of an
  ESP-IDF build and refuses a disabled feature that is still in the image.

## Consequences

**Good**

* The x86-64 kernel stops carrying a PSRAM bookkeeping it can never use.
* The board can drop MQTT, HTTP or NTP for a build that needs the flash, and
  the minimal image is small enough to measure.
* Every feature has one switch and one place that documents its default.

**Bad**

* More build combinations: each flag is a configuration that can be wrong.
  The matrix tests and the map check are the answer, not a promise.
* A flag that is off is invisible at runtime; a person who expects `mqtt`
  in the shell has to read the build, not try the command.
