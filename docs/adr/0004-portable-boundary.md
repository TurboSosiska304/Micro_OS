# ADR 0004 - The portable boundary and the contract of the port

* Status: accepted
* Date: 2026-11-19

## Context

ADR 0001 made the kernel portable: `components/kernel/` is plain C11, and
everything platform specific sits behind `kernel/port.h`. That decision
holds, and the kernel core has never included an ESP-IDF header since.

An audit of the whole tree, however, showed that the boundary is only
enforced for the kernel core itself:

* seven portable components call `k_port_*` directly - the shell reads the
  heap statistics and the console, `/proc` prints the memory of the chip,
  `/dev/tty0` reads the console, the filesystem spins its own lock on top of
  a critical section, telnet counts its clients the same way;
* six portable components contain platform code inside them -
  `mfs_flash.c` (`esp_partition`), `config_nvs.c` (`nvs_flash`),
  `net_esp32.c` and the telnet server (`lwip`), `tft_console.c` (FreeRTOS
  semaphore and the display driver), `wifi_esp32.c` (`esp_wifi`);
* the host build skips `graphics`, `telnet` and the drivers, so a part of
  the "portable" system is never executed on the host;
* the contract itself is one header, and it has no version: a port built
  against another generation of it fails in a way nobody names.

The consequence is not that the code is wrong today. It is that "portable"
is a habit, not a property: the next commit can quietly break it, and the
split of the repository into a kernel and a platform (the next step after
this ADR) would have no line to cut along.

## Decision

### 1. The contract is split by subject

`kernel/port.h` becomes an aggregate of six headers, the way Linux keeps
generic interfaces in `include/linux` and lets each architecture own the
parts it implements in `arch/<arch>/include/asm`:

| Header | Subject |
|---|---|
| `kernel/port/console.h` | bytes out, characters in (blocking and polling) |
| `kernel/port/time.h` | monotonic time since the boot |
| `kernel/port/mem.h` | internal heap, PSRAM, allocation |
| `kernel/port/lock.h` | critical sections, recursive mutexes |
| `kernel/port/power.h` | halt, reset |
| `kernel/port/info.h` | name, architecture, contract version |

A source includes the narrow header it needs; `kernel/port.h` exists for
the code that speaks to the port as a whole (the boot, the tests).

### 1a. A port is a machine; the host and the null are substrates

The contract has four implementations in the tree, and only two of them are
ports in the sense of this ADR: **x86-64** and the **ESP32** run the system on
a machine a person can hold. `ports/port_host` and `ports/port_null` implement
the same contract for another reason:

* the **host** runs the portable kernel as a Linux process: its tasks are
  pthreads, its files and sockets are the POSIX ones, and it exists so the
  tests, the simulator, ASan, TSan, valgrind and the coverage run in seconds
  without QEMU or a board. It is the substrate of the tests, not a product
  target; a machine that is a process is not a machine the system ships on.
* the **null** answers "there is none" to every call: it is the shape of the
  contract with no machine under it, linked by the checks.

The directory names are historical (the host and the null were once ports of a
board-only kernel). The tree keeps them until `1.0.0` is out; a rename to
`ports/host` and `tests/port_null` is a cleanup after it, not a gate. What
matters for the boundary is the rule: a **port** implements the contract on a
machine; a **substrate** implements it so the kernel can be run and checked
somewhere else. `docs/porting.md` and the wiki say the same.

### 2. The contract has a version, checked at boot

`KERNEL_PORT_API_VERSION_MAJOR/MINOR` in `kernel/port/info.h` is the
version of the interface. Every port declares the version it was written
for and asserts at build time that it matches the header; the boot
sequence compares the two again and panics on a major mismatch, warns on a
minor one. This is vermagic of Linux (`include/linux/vermagic.h`), reduced
to what a single address space needs: a pair that was not built together
says so at once, instead of failing far from the cause.

### 3. The layering is a rule with an owner

* `components/kernel/` includes `kernel/*` only. Nothing from `drivers/`,
  `kshell/`, `board/` or `esp_*` may appear there - not even in a comment
  that a header pulls in.
* Upper layers (`kshell`, `init`, `procfs`, `devfs`, `mfs`, `config`, `net`,
  `telnet`, `graphics`) never call `k_port_*`. They use the kernel API:
  `k_sysinfo_*`, `k_console_*`, `k_mutex_*`, `k_reboot`, the VFS, `kmm`.
* Platform code lives in `components/port_esp32/` (or another port), never
  inside a portable component. A portable component that needs a backend
  declares an interface in `kernel/` (`blockdev`, `kvstore`, `net`,
  `render`) and the port implements it.
* Drivers stay platform code. There is no universal GPIO/I2C/SPI
  abstraction: on a board with 100 KB of SRAM the indirection would cost
  more than it buys. The port is a kernel contract, not a HAL.

### 4. The boundary is verified by the build and the checks

* the host build compiles every portable component, with stub drivers;
* `make integrity` grows checks for forbidden includes and for the closure
  of the contract (a port must implement every declared function);
* a "null" port (`ports/port_null`) is compiled in CI: it has no POSIX, no
  ESP-IDF and no display, so anything that leaks a host or a chip detail
  into portable code stops the build.

## Consequences

**Good**

* "Portable" becomes a property that the build enforces, not a habit.
* A new chip or board is a new port directory, not an edit in six
  components - which is what the split into `Micro_OS_kernel` and
  `Micro_OS_esp32` needs.
* The contract version makes an incompatible pair of kernel and platform
  visible at the first seconds of the boot.
* `graphics`, `telnet`, `net` and `config` become testable on the host,
  where the sanitizers are.

**Bad**

* A backend interface costs a call and, in one case, a pointer: the
  memory budget of the board is the limit, so the interfaces are narrow.
* The contract now has two copies of the version (kernel and port) that
  have to be bumped together: an assertion, not a convention, keeps them
  honest.
* Some code moves between components; the history of those files has to be
  followed with `git log --follow`.

## Alternatives

* **Keep one `port.h` and one rule "the kernel is portable"** - rejected:
  the audit shows the rule does not survive the second component.
* **A universal HAL over GPIO, I2C, SPI, ADC and everything** - rejected:
  it moves the cost of the abstraction into the scarcest resource of the
  board (internal SRAM) and makes the drivers harder to read, while the
  portability that is actually needed is at the level of a console, a
  block device, a socket and a frame.
* **Kernel modules loaded at runtime (the Linux model)** - not applicable:
  there is no MMU and no linker on the board; everything is linked into
  one image.

## Status of the work (2026-11-19)

The audit that opened this record is closed: every file it named has moved
and both debt lists of `make integrity` are empty, so the rules apply to the
whole tree without exceptions.

| Was | Is now |
|---|---|
| `mfs/src/mfs_flash.c` (`esp_partition`) | `port_esp32/blockdev_flash.c`, registers `flash0`; MicroFS asks `kernel/blockdev.h` |
| `config/src/config_nvs.c` (`nvs_flash`) | `port_esp32/port_kv_nvs.c` and `port_host/port_kv_file.c`; the configuration asks `kernel/kv.h` |
| `net/src/net_esp32.c`, the sockets of `telnet` (`lwip`) | `port_esp32/port_net_lwip.c`, `port_host/port_net_posix.c`; the services ask `kernel/net.h` |
| `graphics/src/tft_console.c` (FreeRTOS, `drivers/display.h`) | `kernel/render.h`; the panel registers a renderer, the console draws through it |
| `wifi/src/wifi_esp32.c` (`esp_wifi`) | `port_esp32/wifi_radio.c`; the core keeps the table and asks the kernel for the lock and the time |
| `k_port_*` in `kshell`, `procfs`, `devfs`, `mfs`, `telnet` | the kernel doors (`k_sysinfo_*`, `k_console_*`, `k_mutex`, `k_reboot`, `k_clock_set_unix`) |

The contract is at version 1.2 (37 functions), the host build compiles every
portable component, and the console of the display is exercised on a laptop
through a renderer of the test.

The repository stays one tree, as Linux is one tree with `arch/`: the
boundary is held by `[layers]` and `[port]`, not by a repository border. A
kernel source release is an artifact (`git archive` over the kernel and the
host port), not a second repository.
