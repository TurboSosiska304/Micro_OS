# Ports

A port is a directory that implements the contract of `kernel/port/*.h` on a
machine; a board is a description of a machine plus the drivers of its chips.
Two of the four implementations are ports - the board and x86-64; the **host**
is the substrate of the tests and the simulator, and the **null** is the shape
of the contract with no machine under it (ADR 0004). The kernel
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
| shell | UART and TFT | VGA, PS/2 or USB HID, COM1 | stdin/stdout | - |
| MicroFS | the `mfs` partition, 5.56 MiB | the disk, or 512 KB of RAM | a file, 512 KB | - |
| `/proc`, `/dev` | yes | yes | yes | - |
| `/etc` and its store | files, NVS behind them | files, no store | files, a file per key | - |
| `execve` | Xtensa code from `progs`, no protection | ring 3, own page tables | `ENOSYS` (a harness) | `ENOSYS` |
| `fork()` | shared memory | copy on write | shared memory | shared memory |
| disk | the internal flash | `ata0` (bus-master or PIO), `ahci0`, `nvme0`, the stick as `usb0` | - | - |
| network | WiFi, lwIP, telnet, HTTP, MQTT | virtio-net, e1000e, usbnet, DHCP, DNS, HTTPS, httpd, MQTT is off | - | - |
| screen | ST7789v2 and a copy of the console | a framebuffer of 1280x800 of the 8x16 font of the PC (Spleen, 160x50, `font.line_gap`), the VGA text as the fallback | a renderer into a file | - |
| MMU | - (the chip has none) | yes: ring 3, NX/W^X, the policy of ADR 0052, SMEP/SMAP | - | - |
| cores | 2 under FreeRTOS, the kernel on 1 | 2 in the MADT, both with `KERNEL_SMP` (off by default) | `MICRO_OS_CPUS`, 1 by default | 1 |
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
the UART and on the TFT, MicroFS on the flash, the boot and console logs in RAM
(the board has no append volume), the scripts of
`/etc/init.d`, the drivers (display, buttons, buzzer, LED, IR on the pin of the
LED, IMU, RTC, battery, microphone, sound detector, WiFi), deep sleep with the
wake of the timer, of BUTTON C and of the RTC alarm, `telnet` on port 23,
`httpd`, `mqtt`, `http` and `ntp`.

What does not: memory protection (`fork()` is the placeholder of a machine
without an MMU), an update over the network (the A/B update is over USB,
ADR 0022), an IR receiver (the board carries the emitter only; `ir rx` waits
for an element on G26), the enclosure modes, a second user. The alarm of the BM8563 turns the board on from
off; a sleeping board obeys it because the firmware sets the timer of the chip
for the moment of the match, not because the INT of the clock is a wake line.
The flash is 8 MB: two application slots of 1.125 MiB, `progs` 132 KiB and
MicroFS 5.56 MiB (ADR 0022). The details, the pins and the quirks are in
[`docs/ports/esp32/README.md`](../ports/esp32/README.md) and
[`docs/ports/esp32/hardware.md`](../ports/esp32/hardware.md).

## x86-64: bare metal in QEMU

A machine with no operating system under it: no libc, no POSIX, no firmware.
The loader is a multiboot image that QEMU takes with `-kernel`; it copies the
kernel to 16 MiB, maps the same bytes at `0xFFFFFFFF80000000` - the higher half
where the image is linked - builds the identity map of the first gigabyte and
turns long mode on.

```bash
make x86-build
make x86-run            # the window: a framebuffer of 1280x800 and the PS/2 keyboard
make x86-run-serial     # the same machine in a terminal
make x86-smoke          # boot and check the transcript
```

The processors come from the MADT of ACPI, and the boot prints how many the
firmware describes and how many it runs: a kernel built without `KERNEL_SMP`
says `2 cpu(s) reported by the firmware, 1 in use` and names the flag, and one
built with it brings the second processor up through the trampoline of ADR
0012 (`make x86-smp-smoke`). `make x86-smp-build` builds the portable kernel with
`KERNEL_SMP=ON`, and the flag is off by default.

What works: a framebuffer of 1280x800 in RGB565 through the Bochs VBE
interface of the VGA (ADR 0023) - a grid of 160x50 cells of the 8x16 font of
the PC (Spleen), the step of a line from `font.line_gap`, the colours of
`/etc/console`, and the text mode of the VGA when the card has no such
interface - COM1 out of one `k_port_write()`, the
PS/2 keyboard, the PIT, the context switch, the scheduler, paging and ring 3,
NX/W^X (the text of the kernel and of a program is read-only code, every data
page and stack is NX, and the boot audits the tables), the kernel in the
higher half at `0xFFFFFFFF80000000` (ADR 0007),
`execve` with `MOX1` images, `fork()` with copy on write, MicroFS on the first
partition of an ATA disk (or on 512 KB of RAM without one), the modes and the
owners, and the network: the ethernet of a laptop (the `e1000e` of an Intel
I219) or a virtio-net card, ARP, ICMP, UDP, a client and a server of TCP, a
resolver with a cache and a client of DHCP that takes the address of the
network and keeps the lease in `/etc/network` (`make x86-net-smoke`). It is
the machine of [`programs.md`](programs.md) and the proof that the kernel is
portable.

What does not: the key-value store (there is no `/etc` that survives a reset),
Wi-Fi and the touchpad of a laptop, and the parts of the network that
wait for more than one machine: IPv6, a congestion window, and the PHY of a
real I219 (the driver of `e1000e` has its plain half). The details are in
[`docs/ports/x86_64/README.md`](../ports/x86_64/README.md).

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
The cores are a setting of the run: `MICRO_OS_CPUS=2` tells the kernel it has
two, and a task is then in the queue of one of them (`make test-smp`,
`make test-tsan` and `make soak-host` do it). Every task is already a pthread,
so a "core" is a run queue and an identity of the task, not a worker thread.
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
* [programs.md](programs.md) - why only x86-64 runs a program with protection.
* [porting.md](porting.md) - the contract, the bring-up order and the checks.
* [`docs/porting.md`](../porting.md) - bringing the kernel to another machine, in full.

[Back to the wiki index](README.md)
