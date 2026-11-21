# Porting Micro OS to another machine

The kernel is portable: it lives in `components/kernel/` and knows the
platform only through the contract in `kernel/port/*.h` and
`kernel/port_task.h`. A **port** is a directory that implements that
contract; a **board** is a description of a machine plus the drivers of its
chips. Nothing above the port is edited — if a port needs a change in
`components/kernel/`, the boundary is wrong
([ADR 0004](adr/0004-portable-boundary.md)).

The tree carries four ports, and they are the examples:

| Port | What it is | Build |
|---|---|---|
| `ports/port_null` | no machine; every call answers "there is none" | host |
| `ports/port_host` | a Linux process: pthreads, files, sockets | host |
| `ports/port_x86_64` | bare metal in QEMU; loader, IDT, PIT, COM1 | `make x86-build` |
| `components/port_esp32` | ESP-IDF, FreeRTOS, lwIP, NVS, the `mfs` partition | `make idf-build` |

Read them in that order. `port_null` is the shape of the contract with no
machine under it, `port_host` is the same calls over POSIX, `port_x86_64` is
the worked example of programs, signals and an MMU, and `port_esp32` is the
board with its quirks ([ADR 0018](adr/0018-esp32-programs.md)).

## The contract at version 1.8

`KERNEL_PORT_API_VERSION_MAJOR/MINOR` in
[`kernel/port/info.h`](../components/kernel/include/kernel/port/info.h) is
the vermagic of the pair: the port declares the version it was written for,
a `_Static_assert` compares it at build time, and the boot compares it again.

The version 1.8 contract is **54 functions in ten headers**: 45 in
`kernel/port/*.h` and nine in `kernel/port_task.h`. `kernel/port.h` is the
aggregate of the nine subject headers and does not include the task
substrate; a port includes both.

| Header | Functions | What it is | Mandatory? |
|---|---|---|---|
| `kernel/port_task.h` | 9 | stack, tasks, sleep, signals | **yes** |
| `kernel/port/console.h` | 4 | the byte sink; a line, a key, a poll | **yes**; input optional |
| `kernel/port/time.h` | 2 | milliseconds and microseconds since boot | **yes** |
| `kernel/port/mem.h` | 6 | the heap, the PSRAM and their numbers | **yes**; PSRAM may be absent |
| `kernel/port/lock.h` | 6 | critical sections and recursive mutexes | **yes** |
| `kernel/port/power.h` | 2 | halt and reset | **yes** |
| `kernel/port/info.h` | 4 | name, architecture, contract version | **yes** |
| `kernel/port/kv.h` | 5 | the store behind `/etc` | no: "nothing is stored" |
| `kernel/port/net.h` | 8 | TCP client and server, one UDP exchange | no: "no network" |
| `kernel/port/exec.h` | 8 | the window of programs, the entry, the end | no: "no programs" |

The execution substrate is not optional: a port that implements the nine
subject headers but not `kernel/port_task.h` has no context for the
scheduler to switch to.

### What "optional" means

An absent duty is a group of stubs that answer honestly, not a missing
symbol. The null port is the reference; these are its answers, and a real
port may improve on them without changing the shape:

* `kv` — `get` and `next` answer `-2`, `set` `-1`, `erase` `-2`,
  `commit` `0`.
* `net` — `connect` and `listen` return `NULL`; `send`, `recv` and
  `udp_exchange` answer `-1`; `close` and `server_close` do nothing.
* `exec` — `supported` is `false`, `arch` is `-1` (or the machine of the
  format when the processor has one), `base` is `NULL`, `enter` answers
  `-1`, `active` is `false`, `exit` does nothing, `contains` is `true`,
  `fork_copy` answers `-1`.

The kernel turns each of these into the proper answer of a system call:
`ENOSYS` for a program on a machine that runs none, `ENOENT` for a key that
was never stored, a connection that fails instead of a crash.

### Signal frames

`k_port_task_signal_enter()` and `k_port_task_signal_leave()` are the seam of
`signal()` and `sigreturn` ([ADR 0015](adr/0015-signals.md)). A port whose
programs are tasks of the kernel may answer `0` from `enter` (the kernel runs
the handler itself) and `-1` from `leave`; a port with a user context saves
the interrupted frame and answers `1`. The three answers are a contract:
`1` entered a user frame, `0` has no user context, `-1` cannot carry a frame
and the task has to end.

## The duties that are not `k_port_*` calls

Two platform duties have no function of the contract:

* **The boot.** The port calls `kernel_boot()` with a
  `struct kernel_boot_config` (`kernel/boot.h`): the board name, its
  description, the table of subsystems and their levels. It also registers
  `struct kernel_board` (`kernel/board.h`) so that the banner, `uname` and
  `mfetch` name the same machine.
* **The block devices.** A filesystem consumes blocks, not a flash chip: the
  port fills a `struct k_blockdev` (`kernel/blockdev.h`) with a sector size,
  a count and `read`/`write`/`erase`/`sync`, and calls
  `k_blockdev_register()`. The structure and its context live as long as the
  system; the kernel keeps the pointer. The partition table above it is the
  kernel's business.

Drivers are not port duties either: a driver registers a `kmod` module and,
for a display, a renderer. The port talks to the kernel, not to a driver.

## Bringing up a new machine

The order below is the one that hurts least, and it is the order the x86-64
port took. At every step `make test` and `make integrity` must stay green on
the host, because the host build compiles the kernel against `port_null` as
well.

1. **Console.** Get one character out before anything else. Every later step
   is debugged through it. Nothing else has to work; the input calls may
   answer `-1` until there is a keyboard.
2. **Time.** A counter that only goes forward. Without it the log has no
   timestamps and the scheduler has no ticks.
3. **Execution substrate.** `k_port_task_*` over whatever the machine has:
   FreeRTOS, pthreads, or a switch of your own. The kernel's tests then run.
4. **Memory.** `k_port_alloc` over the heap of the chip, and the two pairs of
   numbers. `kmm` starts working, and with it the VFS.
5. **Locks.** The critical section first (interrupts off, a few
   instructions), then the recursive mutex, which needs the substrate of
   step 3 to put a task to sleep.
6. **Block device.** Register the flash, the disk or the card with
   `k_blockdev_register`; MicroFS can then mount. A machine with no storage
   may skip this step and run on `/proc`, `/dev` and `/etc` alone.
7. **Store, network, programs.** In that order, and each one may say "I have
   none": the store is NVS, a file or nothing; the network is lwIP, a stack
   of your own or nothing; programs need a window and, for protection, an
   MMU. The answers are in the table above.
8. **Board and drivers.** `components/board` says what the machine is; the
   drivers register their `kmod` modules and the renderer of the display.

## The checks a port has to pass

`make integrity` has the groups that matter to a port:

* `[layers]` — the kernel includes only the kernel; a portable component
  includes no platform header and calls no `k_port_*`; the host build
  includes every portable component;
* `[port]` — every function declared in `kernel/port/*.h` has an
  implementation in every port directory (the built library for the host and
  null ports, the text of the sources for the ESP32 and x86-64 ports), so a
  half-written port cannot hide;
* `[budget]` — the image stays inside its 1.2 MB budget;
* `[sources]`/`[format]` — UTF-8, LF, no tabs, 100 columns for C, clang-format.

The rest:

```bash
make test                 # the whole portable kernel against port_host, plus port_null
make ci                   # build, tests, ASan, the simulator smoke, integrity
make smoke                # the simulator boots and answers
make x86-smoke            # the bare x86-64 machine boots (QEMU)
make x86-disk-smoke       # programs and a file survive a reset
make x86-net-smoke        # the network of the bare machine
tools/device-test.sh /dev/ttyACM0   # the board: build, flash, RESULT: PASS
```

A successful boot says which contract it speaks:

```text
port: host, contract 1.8
```

A **major** mismatch panics with the two versions; a **minor** one warns (the
linker has already refused a call the port does not implement).
`make integrity` does not compile the ESP32 port, so a port of a board also
needs its own build and its own smoke test.

## Minimal checklist

* [ ] a directory with a build file, compiled by one of the builds (or a
      build of its own, like x86-64);
* [ ] `info`: the name, the architecture, `1.8`;
* [ ] `port_task`: a stack size, create/start/yield/sleep/exit/current, and
      the two signal calls (a stub is allowed only if the kernel runs
      handlers itself);
* [ ] `console`: `write` first, then the three input calls;
* [ ] `time`: two monotonic counters;
* [ ] `mem`: alloc/free and the four numbers; the PSRAM may be the heap;
* [ ] `lock`: a critical section that does not sleep, a recursive mutex that
      does;
* [ ] `power`: halt and reset, neither returns;
* [ ] `kv`, `net`, `exec`: real or honest stubs (see the table);
* [ ] `kernel_boot()` with the board description and the subsystem table;
* [ ] the block devices the machine has, registered before MicroFS mounts;
* [ ] `make test`, `make integrity`, and the smoke test of the new machine;
* [ ] a `docs/ports/<name>/README.md` with the quirks of the machine.

## Common mistakes

* **errno.** The kernel returns negative `KERR_*` values
  (`kernel/errno.h`); it has no libc `errno` to set. A port does not touch
  `errno` either. If the machine runs programs, the library's `_Thread_local`
  errno has to follow the task: the x86-64 port copies the TLS block per task
  and points `IA32_FS_BASE` at it. The board still has one errno for all tasks
  — a known debt, not a pattern to copy.
* **Critical sections.** Interrupts off for a few instructions only: no
  sleeping, no allocation, no `klog`, no network. The recursive mutex is the
  lock for a subsystem and may sleep. `k_port_mutex_destroy()` answers `-1`
  while a task holds or waits for the lock; the caller keeps the handle.
* **Console in an interrupt.** `k_port_write()` can be called from the log,
  from a fault and from an interrupt. It must not block on a full UART or on
  a lock held by a task that is asleep.
* **Time.** Never let a counter go backwards, and do not derive milliseconds
  from a wall clock that can be set. The RTC is a driver, not a port duty.
* **Memory.** `psram` is a preference, not a promise: a port without PSRAM
  returns `0`/`0` and may serve the request from the internal heap. Return
  `NULL` on failure; `k_port_free(NULL)` is a no-op.
* **Network timeouts.** All three timeouts are milliseconds. `recv` answering
  `0` is a peer that closed the session, `-1` is an error or a timeout; a
  port that mixes them up makes `telnet` spin or hang. `send` with `wait`
  false may drop the tail on purpose.
* **Program windows.** The window is fixed for the life of the port. Copy
  the image, clear the bss, pass `argv` and `image->envp`, and return `-1`
  before a byte runs when the image does not fit. `contains()` is the seam of
  `copy_from_user`: it checks a range page by page and answers `false` for
  the null page, the guard page and an unmapped range.
* **fork and exit.** `k_port_fork_copy()` answers the child pid, `-1` for a
  machine without an address space per task, `-2` when the copy is out of
  memory. A program's `exit` is not a kernel task's exit: `k_port_exec_exit()`
  returns to `k_port_exec_enter()` with the status, and `active()` says which
  of the two the kernel is looking at.
* **Block devices.** The `struct k_blockdev` and its context must live
  forever; a sector is the unit of the device, not of the filesystem; `erase`
  and `sync` may be `NULL` when the medium does not need them.
* **The store.** A key is at most 16 bytes and a value at most 128. A buffer
  that is too small is an error, never a truncated value: the caller would
  save the truncation back as the truth.
* **The boundary itself.** A port does not call `k_port_*` from a portable
  component, does not include a driver header, and does not know the syscall
  ABI. If the kernel needs a new door, that is a new call of the contract and
  a bump of the version for every port.

## What a port does not do

* It does not add a call to the contract for one board. If the kernel needs
  something new, it is a new door in `kernel/*.h` and a bump of the contract,
  which is a change for every port — that is the point of the version.
* It does not include a driver header. The display, the buttons and the radio
  are drivers; a port talks to the kernel, not to them.
* It does not know the syscall ABI. Programs do, through `kernel/ksys.h`.

## Where to read next

* [ADR 0004](adr/0004-portable-boundary.md) — why the boundary exists and
  what the checks enforce.
* [ADR 0007](adr/0007-paging-and-user-mode.md) — paging, ring 3, the failure
  of a program and the signal frame of the x86-64 port.
* [ADR 0018](adr/0018-esp32-programs.md) — a machine without an MMU that
  still runs a program, and what the port gets after 1.0.
* `docs/ports/` — one directory per port, with its notes and its plan.
* The wiki: [build](wiki/build.md), [testing](wiki/testing.md) and
  [ports](wiki/ports.md).
