# Micro OS kernel roadmap

> The Russian version is not written yet; the English text is the
> canonical one, as everywhere in the repository.

This is the roadmap of **the kernel**: the part of the tree that does not
know what it runs on. The board, the drivers, the radio and the firmware
have their own roadmap in [roadmap.md](roadmap.md).

The split is a matter of directories and checks, not of repositories: the
kernel lives in `components/kernel/`, the portable services beside it, the
host machine in `ports/port_host/`, and the boundary is held by
`make integrity` (`[layers]`, `[port]`) and by the host build - see
[ADR 0004](adr/0004-portable-boundary.md).

Every item ends with a state on the board and green tests.

## K0 - Portable core (done)

* [x] Boot levels, log with a ring buffer, panic, version, build identity
* [x] Board description as kernel data (`kernel/board.h`, registered at boot)
* [x] Task API and scheduler on the substrate of the port, PIDs, parents
* [x] The syscall ABI (`kernel/ksys.h`), frozen numbers, one door to the kernel
* [x] VFS with mount points, `/proc`, `/dev`, a descriptor that remembers its access
* [x] `kmm`: slabs for the small blocks, the platform heap for the large ones
* [x] MicroFS as a consumer of `kernel/blockdev.h`, not of a flash chip
* [x] The libc subset over the ABI, `fork()` in the shape of POSIX

## K1 - The contract of the port (done)

* [x] One header per subject: console, time, memory, locking, power, store, sockets
* [x] The version of the contract is the vermagic of the pair, checked at boot
* [x] `k_mutex` (recursive, blocks) beside the critical section (may not block)
* [x] Doors for the upper layers: `kernel/sysinfo.h`, `kernel/kv.h`,
      `kernel/net.h`, `kernel/clock.h`, `kernel/render.h`
* [x] The closure of the contract under test (`tests/cases/test_port.c`)
* [x] `[layers]` and `[port]` in `make integrity`: a leak fails the build
* [x] The host port as the machine of the tests (`ports/port_host`)

## K2 - The host machine (done)

* [x] `ports/port_null`: a port with no POSIX and no chip, linked by the host
      build - the proof that nothing portable depends on either
* [x] The stubs of the board for the host: the doctor modules, the commands,
      the clock of the process, a renderer that draws into a file
* [x] `make sim` boots the whole system: MicroFS on a file volume, `/proc`,
      `/dev`, `/etc` restored from the store, `init`, then the shell
* [x] A scenario test of the simulator (`make smoke`), on a volume and a store
      of its own
* [x] A bare-metal machine in the tests: x86-64 in QEMU (`make x86-smoke`)
* [ ] The simulator on the loopback: `http`, `ntp` and `telnet` against a
      server of the test (the sockets of the host are exercised by the unit
      tests of `net`, not by the simulator)

## K3 - Programs (planned)

* [ ] A format of a program and a loader: the header, the segments, the entry
* [ ] `execve` over the syscall ABI, the first program in `/bin`, `PATH`
* [ ] The arguments and the environment as the kernel hands them over
* [ ] A program that fails does not take the system with it (as far as a
      system without an MMU can promise)

## K4 - Protection (planned)

* [ ] Mode bits and owners in MicroFS; `chmod`, `chown`, the check at `open`
* [ ] `fork()` with an address space of its own, copy on write
* [ ] The checks of every argument at the edge of the syscall table

## K5.0 - A second machine in the tests (done, except paging)

The first bare-metal port is x86-64, not ARM: the machine is documented,
the console is a UART that predates the PC, and QEMU boots the kernel in
CI. The port is `ports/port_x86_64`, the boot is the project's own
(multiboot: `loader32.S` copies the kernel and turns the long mode on).

* [x] The contract of the port 1.2 on x86-64: COM1, the TSC against the
      PIT, a first fit heap over the memory map, the honest stubs of the
      store and the sockets
* [x] The banner of the kernel on a real (emulated) processor, and
      `make x86-smoke` in the checks
* [x] The IDT and the timer interrupt, a kilohertz, the ticks as the clock
* [x] The task substrate: the switch of the contexts (ADR 0002)
* [x] The shell on the serial port, with `help`, `uname`, `uptime`, `ps`
* [ ] Paging with the kernel in the higher half (the identity map is the
      first step, not the model)

## K5 - A processor with an MMU (planned)

* [ ] A port for a real ARM: the page tables, the switch of the address space,
      the protection of the kernel memory
* [ ] `KERNEL_NOMMU` becomes 0 and the concessions disappear: the logical
      boundary becomes a hardware one, `fork()` a copy
* [ ] The same sources, the same contract, a second `arch/`

## K6 - The filesystem and the network (planned)

* [ ] A second block device and a second filesystem behind the VFS (FAT on a
      card, or a RAM volume that is not MicroFS)
* [ ] Timestamps of a file, the clock of the kernel as their source
* [ ] The protocols above the sockets: MQTT for the sensors, a page of the
      state over HTTP

## K7 - Quality (planned)

* [ ] The races of the host port under TSan (the critical sections are a no-op
      there, ADR 0001)
* [ ] A fuzz of the syscall ABI and of the parsers of the filesystem
* [ ] The budget of the kernel: the size of the image and the SRAM, checked at
      every release against a known number

## Not in the kernel roadmap

The board bring-up, the drivers, the radio, the deep sleep, the microphone,
the IR emitter, OTA and the flash layout are the business of the platform:
see [roadmap.md](roadmap.md).
