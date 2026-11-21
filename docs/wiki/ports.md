# Ports

A port is a directory that implements the contract of `kernel/port/*.h`; a
board is a description of a machine plus the drivers of its chips. The kernel
in `components/kernel/` includes nothing of a platform, and `make integrity`
fails when it tries. That is why the same sources build as a Linux process, as
a bare-metal x86-64 machine and as the firmware of the board; the reasoning is
in [ADR 0004](../adr/0004-portable-boundary.md), and the recipe for a new
machine is in [`docs/porting.md`](../porting.md).

## What runs where

`yes` means it is there and used, `-` means it is absent, and a note in
parentheses is the reason.

| | ESP32 (board) | x86-64 (QEMU) | host (simulator) | null |
|---|---|---|---|---|
| shell | UART and TFT | VGA, PS/2, COM1 | stdin/stdout | - |
| MicroFS | the `mfs` partition, 5.75 MB | the disk, or 512 KB of RAM | a file, 512 KB | - |
| `/proc`, `/dev` | yes | yes | yes | - |
| `/etc` and its store | files, NVS behind them | files, no store | files, a file per key | - |
| `execve` | `ENOSYS` (no executable flash) | ring 3, own page tables | `ENOSYS` (a harness) | `ENOSYS` |
| `fork()` | shared memory | copy on write | shared memory | shared memory |
| disk | the internal flash | ATA PIO, `ata0` | - | - |
| network | WiFi, lwIP, telnet, HTTP, MQTT | - | - | - |
| panel | ST7789v2 and a copy of the console | VGA text | a renderer into a file | - |
| MMU | - (the chip has none) | yes, used for programs | - | - |
| build | `make idf-build` | `make x86-build` | `make build` | by the host build |

The matrix follows the ports, not a wish: the board has no MMU and its flash is
not executable for a file, so it cannot run a program; the simulator must not
jump into an arbitrary image, so it cannot either; the x86-64 machine has the
loader, the page tables and the ring that make it possible. Everything else -
the scheduler, the VFS, MicroFS, `/proc`, `/dev`, the libc subset, the shell -
is the same code on all of them.

## ESP32: the board

The M5Stack StickC Plus2 (`ESP32-PICO-V3-02`, 240 MHz, 2 MB PSRAM, 8 MB flash,
135x240 TFT), on ESP-IDF v5.3 and FreeRTOS.

```bash
make idf-build
make idf-flash IDF_PORT=/dev/ttyACM0
tools/device-test.sh /dev/ttyACM0      # build, flash, read, RESULT: PASS
```

What works: the boot with the self test and the hardware doctor, the console on
the UART and on the TFT, MicroFS on the flash, `/log/start.log`, the scripts of
`/etc/init.d`, the drivers (display, buttons, buzzer, LED, IMU, RTC, battery,
microphone, WiFi), `telnet` on port 23, `httpd`, `mqtt`, `http` and `ntp`.

What does not: memory protection (`fork()` is the placeholder of a machine
without an MMU), running a program out of the flash, OTA, the IR driver, the
sound detector, the enclosure modes, a second user. The RTC alarm turns the
board on from off; it does not wake a chip that is already asleep (the INT is
wired to the power latch, the notes say so). The flash is 8 MB: `factory` 2 MB and
MicroFS 5.75 MB, with no OTA slot. The details, the pins and the quirks are in
[`docs/ports/esp32/README.md`](../ports/esp32/README.md) and
[`docs/ports/esp32/hardware.md`](../ports/esp32/hardware.md).

## x86-64: bare metal in QEMU

A machine with no operating system under it: no libc, no POSIX, no firmware.
The loader is a multiboot image that QEMU takes with `-kernel`; it copies the
kernel to 16 MiB, builds the identity map of the first gigabyte and turns long
mode on.

```bash
make x86-build
make x86-run            # the window: VGA text and the PS/2 keyboard
make x86-run-serial     # the same machine in a terminal
make x86-smoke          # boot and check the transcript
```

What works: the VGA text console and COM1 out of one `k_port_write()`, the
PS/2 keyboard, the PIT, the context switch, the scheduler, paging and ring 3,
`execve` with `MOX1` images, `fork()` with copy on write, MicroFS on the first
partition of an ATA disk (or on 512 KB of RAM without one), the modes and the
owners. It is the machine of [`programs.md`](programs.md) and the proof that
the kernel is portable.

What does not: the network (`k_port_net_*` says no), the key-value store (no
`/etc` to persist), a second disk, DMA, the kernel in a higher half. The
details are in [`docs/ports/x86_64/README.md`](../ports/x86_64/README.md).

## host: the tests and the simulator

A Linux process: threads instead of a scheduler substrate, `pthread` mutexes
instead of a spinlock, files and sockets instead of a flash and a radio. It is
the development loop and the machine of `make test`.

```bash
make test        # the unit tests, plus the null probe
make sim         # the whole system on a volume in a file
make smoke       # the same, driven by a script
```

What works: everything portable, with the drivers stubbed and the console
rendered into a file or nowhere. What it cannot do: run a program, and
reproduce preemption - the critical section is a recursive `pthread` mutex, so
a race is invisible unless the race detector looks for it (`make test-tsan`).
The details are in [`docs/ports/host/README.md`](../ports/host/README.md).

## null: the port that is not a machine

`ports/port_null` answers "there is no such thing" to every call: no console,
no flash, no store, no sockets, no tasks. It is never run for a purpose. It
exists for one check: `microos_null_probe` links the kernel **and every
portable component** against it, so a symbol that portable code takes from a
machine by accident stops the build on a laptop. It is compiled by the host
build and run by `ctest`, and it is the compiled form of ADR 0004.

It cannot catch a libc call - the probe is linked with the libc of the host -
but it catches a leak of the port itself, which is the part that has to be
complete. The details are in
[`docs/ports/null/README.md`](../ports/null/README.md).

## Which page to read next

* [build.md](build.md) - the commands and the dependencies of each build.
* [testing.md](testing.md) - what each machine is checked with.
* [programs.md](programs.md) - why only x86-64 runs a program.
* [`docs/porting.md`](../porting.md) - bringing the kernel to another machine.

[Back to the wiki index](README.md)
