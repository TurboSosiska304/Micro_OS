# Porting Micro OS to another machine

The kernel is portable: it lives in `components/kernel/` and knows the
platform only through the contract in `kernel/port/*.h`. A port is a
directory that implements that contract, and a board is a description plus
the drivers of its chips. Nothing above the port is edited — if a port needs
a change in `components/kernel/`, the boundary is wrong (ADR 0004).

Three ports exist, and they are the examples:

| Port | What it is |
|---|---|
| `components/port_esp32` | the board: ESP-IDF, FreeRTOS, lwIP, NVS, the partition of MicroFS |
| `ports/port_host` | a Linux process: the machine of the tests and the simulator |
| `ports/port_x86_64` | a bare-metal x86-64 machine in QEMU: its own loader, IDT and PIT, a context switch, the shell on COM1 |
| `ports/port_null` | no machine at all: built and linked in CI to prove the kernel needs nothing else |

The first three are in the host build and in the checks; the x86-64 port is a
project of its own (`make x86-build`, `make x86-smoke`), because it needs QEMU
and a loader rather than the host compiler alone.

## What a port has to provide

The contract is 43 functions in nine headers, and the execution substrate.
Every function is documented at its declaration; the table is the summary.

| Header | Functions | What it means on a new machine |
|---|---|---|
| `kernel/port/console.h` | `write`, `console_readline`, `console_getchar`, `console_poll` | a UART, a terminal, or nothing |
| `kernel/port/time.h` | `uptime_ms`, `uptime_us` | a free-running counter since the boot |
| `kernel/port/mem.h` | `heap_free/total`, `psram_free/total`, `alloc`, `free` | the heap of the chip; "PSRAM" may be the same heap or absent (return 0 and say 0 total) |
| `kernel/port/lock.h` | `enter/exit_critical`, `mutex_create/destroy/lock/unlock` | interrupts off for a few instructions, and a blocking recursive lock |
| `kernel/port/power.h` | `halt`, `reset` | a loop with the interrupts off, and the reset of the board |
| `kernel/port/kv.h` | `kv_get/set/erase/commit/next` | a place for a handful of strings that survives a restart, or "there is none" |
| `kernel/port/net.h` | `net_connect/send/recv/close`, `net_listen/accept/server_close`, `net_udp_exchange` | TCP and UDP, or stubs that answer "no network" |
| `kernel/port/info.h` | `name`, `arch`, `api_major`, `api_minor` | two words about the platform and the version of the contract |
| `kernel/port/exec.h` | `exec_supported`, `exec_arch`, `exec_base`, `exec_enter`, `exec_active`, `exec_exit`, `exec_contains`, `fork_copy` | whether a program can be loaded and run, which machine of the format the port is (0 x86-64, 1 Xtensa, -1 for a port that runs none), where, the entry itself, whether a program is in flight, how it ends, whether a range is the memory of a program, and whether a fork can copy the context of a program (the answer is -1 on a machine without an address space per task) |
| `kernel/port_task.h` | `stack_size`, `task_create/start/yield/sleep_ms/exit/current` | the execution substrate: FreeRTOS, pthreads, or a switch of your own |

A port that implements the headers but not `kernel/port_task.h` is not a
port: the scheduler has no context to switch. The x86-64 port is the worked
example of all of it: `loader32.S` copies the kernel and turns the long mode
on, `interrupts.S` and `switch.S` are the two pieces of assembly, and the rest
is C against the contract.

## The contract has a version

`KERNEL_PORT_API_VERSION_MAJOR/MINOR` is the vermagic of the pair. A port
declares the version it was written for and asserts it at build time; the
boot compares it again and panics on a major mismatch. Bump the minor when
a function is added, the major when the meaning of one changes.

## Bringing up a new machine, in the order that hurts least

1. **Console.** Get one character out before anything else; every later step
   is debugged through it. Nothing else has to work.
2. **Time.** A counter that only goes forward. Without it the log has no
   timestamps and the scheduler has no ticks.
3. **Execution substrate.** `k_port_task_*` over whatever the machine has
   (FreeRTOS, a scheduler of its own, a thread library). The kernel's own
   tests then run: `make test` on the host builds the same sources.
4. **Memory.** `k_port_alloc` over the heap of the chip, and the two pairs of
   numbers. `kmm` starts working, and with it the VFS.
5. **Locks.** The critical section first (interrupts off), then the recursive
   mutex, which needs the substrate of step 3 to put a task to sleep.
6. **Store, block device, network.** In that order, and each one may say "I
   have none": the configuration lives in `/etc` alone, the filesystem is the
   RAM device of the tests, and the services answer that the board is alone.
7. **Board and drivers.** `components/board` says what the machine is; the
   drivers register their `kmod` modules and a renderer for the display.

At every step the host build must stay green: `make ci` compiles the kernel
against `port_null` as well, so a dependency that creeps in breaks CI before
it reaches the machine.

## What the checks will tell you

`make integrity` has four groups that matter to a port:

* `[layers]` — the kernel includes only the kernel; a portable component
  includes no platform header and calls no `k_port_*`; the host build
  includes every portable component;
* `[port]` — every function of the contract has an implementation in every
  port directory, so a half-written port cannot hide;
* `[budget]` — the image stays inside its 1.2 MB budget;
* `[sources]`/`[format]` — the usual hygiene.

## What a port does not do

* It does not add a call to the contract for one board. If the kernel needs
  something new, it is a new door in `kernel/*.h` and a bump of the contract,
  which is a change for every port — that is the point of the version.
* It does not include a driver header. The display, the buttons and the radio
  are drivers; a port talks to the kernel, not to them.
* It does not know the syscall ABI. Programs do, through `kernel/ksys.h`.
