# Micro OS architecture

> Russian version: [architecture_ru.md](ru/architecture_ru.md)

## 1. Overview

The kernel is one portable core; a machine is a port under it; a board is
the description of the machine plus the drivers of its chips.

```
+---------------------------------------------------------------------+
| Programs: the shell, the commands, the daemons of /etc/init.d       |
+---------------------------------------------------------------------+
| API: POSIX-like (open/read/write/fork/...) over kernel/ksys.h       |
+---------------------------------------------------------------------+
| Kernel (plain C11, no hardware):                                    |
|   sched    task scheduler (PCB, states, PIDs, priorities)            |
|   vfs      inode, dentry, mount, /proc, /dev                         |
|   kmm      memory manager (slabs, the heap of the port)              |
|   ksys     the syscall table and its ABI                             |
|   mfs      MicroFS, a consumer of kernel/blockdev.h                  |
|   kshell   the command interpreter; its output is kernel/console.h   |
+---------------------------------------------------------------------+
| The port contract (kernel/port/*.h): console, time, memory, locks,   |
| power, store, sockets, identity; and the execution substrate        |
+---------------------------------------------------------------------+
| Ports:                                                               |
|   port_esp32   ESP-IDF + FreeRTOS + lwIP + NVS, the partition of MFS |
|   port_host    a Linux process (pthreads, files, sockets)           |
|   port_x86_64  bare metal on QEMU: its own loader, IDT/PIT, switch  |
|   port_null    no machine at all, linked by the checks              |
+---------------------------------------------------------------------+
| Boards and drivers (the ESP32 port): the StickC Plus2, the panel,   |
| the buttons, the IMU, the RTC, the battery, the radio               |
+---------------------------------------------------------------------+
```

Rule number one: **the kernel knows nothing about hardware**. Everything that
depends on the platform sits behind the `kernel/port.h` contract, which is
split by subject the way Linux keeps `include/linux` apart and lets each
architecture own its part in `arch/<arch>/include/asm`:

| Header | Subject | Examples |
|---|---|---|
| `kernel/port/console.h` | bytes out, characters in | `k_port_write`, `k_port_console_getchar`, `k_port_console_poll` |
| `kernel/port/time.h` | time since the boot | `k_port_uptime_ms`, `k_port_uptime_us` |
| `kernel/port/mem.h` | internal heap and PSRAM | `k_port_alloc`, `k_port_heap_free` |
| `kernel/port/lock.h` | critical sections, recursive mutexes | `k_port_enter_critical`, `k_port_mutex_lock` |
| `kernel/port/power.h` | halt, reset | `k_port_halt`, `k_port_reset` |
| `kernel/port/kv.h` | the store that survives a restart | `k_port_kv_get`, `k_port_kv_next` |
| `kernel/port/net.h` | TCP and UDP | `k_port_net_connect`, `k_port_net_listen` |
| `kernel/port/info.h` | identity and contract version | `k_port_name`, `k_port_api_major` |
| `kernel/port/exec.h` | running a program | `k_port_exec_supported`, `k_port_exec_base`, `k_port_exec_enter` |

The version of the contract (`KERNEL_PORT_API_VERSION_MAJOR/MINOR`) is the
vermagic of the pair: the port asserts it at build time and the boot
sequence checks it again, so a kernel and a port that were not built
together say so instead of failing far from the cause. See
[ADR 0004](adr/0004-portable-boundary.md).

Above the contract sit the doors the upper layers use, so that no component
outside the kernel includes `kernel/port.h` at all: `kernel/sysinfo.h`,
`kernel/console.h`, `kernel/kv.h`, `kernel/net.h`, `kernel/clock.h` and
`kernel/render.h`. A leak of the port into the components above fails
`make integrity` (`[layers]`), together with any ESP-IDF header that appears
where it does not belong.

That is exactly what allows the kernel tests to run on a laptop.

## 2. What is Linux-like here, and what is not

| Property | Linux | Micro OS |
|---|---|---|
| Modular monolith | yes | yes (kernel + drivers in one address space) |
| Layered subsystems | yes | yes (`kernel/boot.h`, initialisation levels) |
| Processes / threads | yes | yes, simplified, from phase 1 |
| Filesystem | yes | yes, from phase 2 (in-RAM FS plus a VFS layer) |
| `/proc`, `/dev`, `/sys` | yes | `/proc` and `/dev` from phase 2, `/sys` later |
| System calls | yes | yes, from phase 3, but **without hardware isolation** |
| Memory protection | MMU | **none used**: the ESP32 has no MMU, and the x86-64 port has one
  but keeps the identity map of its loader and the logical boundary |
| Multi-user | yes | no (a single user: the device itself) |
| Dynamic linker | yes | no (everything is statically linked) |

We honestly call this **Linux-like**, not Linux. On the board there is no
MMU: the separation of user and kernel space is logical, system calls are
ordinary function calls with a uniform ABI, and the boundary is kept by
discipline. The x86-64 port has an MMU and does not use it yet - the identity
map of its loader is the first step, and the address spaces of the tasks are
the next one (see `docs/roadmap.md`).

That is a property of the current port, not the shape of the kernel: everything
the kernel concedes to a board without an MMU sits behind one compile time flag,
`KERNEL_NOMMU` in `kernel/config.h`. A port for a processor with an MMU turns the
flag off, and the same sources start offering what it takes away - an address
space of its own, a real `fork()`, protection at the edge of the system call.

## 3. Boot model

Booting mirrors the Linux model: subsystems are registered with a **level** and are
initialised in level order.

| Level | Contents | Examples |
|---|---|---|
| `CORE` (0) | critical subsystems | log, board, memory |
| `DRIVER` (10) | hardware drivers | tft, input, buzzer, imu, rtc |
| `SERVICE` (20) | system services | vfs, procfs, devfs |
| `APP` (30) | applications | init, shell |

Every subsystem returns 0 or a negative error code. A failed initialisation
triggers a panic that names the subsystem, so a broken driver is diagnosed
immediately instead of causing mysterious crashes later.

Phase 0 already implements this mechanism (`components/kernel/src/boot.c`),
including level ordering and error checking.

## 4. Scheduler (phase 1)

* `struct task` — the analogue of `task_struct`: state, priority, stack, TID,
  quantum counter, file descriptors, working directory, environment.
* States: `TASK_NEW -> TASK_READY -> TASK_RUNNING -> TASK_SLEEPING -> TASK_ZOMBIE`.
* Priorities 0..`TASK_PRIO_MAX`; scheduling is preemptive round-robin with
  priorities, like Linux `SCHED_RR`.
* Timer: 1 ms tick (FreeRTOS tick or `esp_timer`), `sleep()` and `usleep()` are
  implemented on top of a timer queue.
* PIDs are handed out starting from the init process (as in Linux), and
  `/proc/<pid>` are virtual directories.

### Execution substrate

Preempting a task at an arbitrary point requires a context switch. On ESP32 there
are two options:

1. **FreeRTOS as substrate** — every kernel task is a FreeRTOS task, priorities
   map one to one. Fast and reliable, but preemption is not "ours".
2. **Our own scheduler on top of FreeRTOS** — FreeRTOS provides a single
   low-level task (or an ISR trigger) and we switch contexts ourselves by
   saving and restoring stack pointers and registers.

Plan: start with option 1 (it works immediately), move to option 2 when real
Linux-like semantics are needed (priorities, states, `fork`). The decision is
recorded in [ADR 0002](adr/0002-scheduler-substrate.md).

## 5. Virtual filesystem (phase 2)

The goal is "everything is a file":

```
/            root (MicroFS on the flash: /etc, /log, ...)
/proc        virtual FS: version, uptime, meminfo, tasks, self/
/dev         devices: tty0 (console), null, input (buttons A/B/C)
/sys         (later) driver parameters
```

Implementation: `inode` -> `dentry` -> `file_operations` (open/read/write/ioctl),
as in Linux, but with a flat dentry list inside a single binary tree.

The configuration lives in the files of `/etc`: a file name is the key and the
content is the value. The NVS keeps a copy of every file, so `mkfs -y` does not
take the settings away — the init process restores the missing files at the
boot and saves the ones it finds, and the file wins when the two disagree (see
`components/config`).

### Who may work with the filesystem at the same time

The volume is one piece of state - the map of the free blocks, the table of
the inodes and the cache of the sector of the header all describe the same
storage - so two tasks that write files at the same time have to be kept
apart:

* every open file belongs to the one that opened it: the position and the
  flags of the file live in the descriptor (`VFS_FILE_DATA_SIZE` bytes of
  private data), not in a table of the filesystem. Two tasks that read
  their own files never touch each other's position;
* the filesystem itself has one lock, taken by every public call of
  `mfs_*`. It is a recursive `k_mutex` of the kernel (`kernel/lock.h`),
  so a task that has to wait is put to sleep by the platform and does not
  burn a time slice looking again; it is recursive because the functions
  of the filesystem call each other (`mfs_close` puts the changes on the
  flash through `mfs_sync`);
* the adapter between the filesystem and the VFS
  (`components/mfs/src/mfs_vfs.c`) is the only place where the codes of
  the errors of the filesystem are translated into the codes of the
  contract of the system calls: the two sets are close but not the same
  (`-9` is "the directory is not empty" of the filesystem and a "bad file
  descriptor" of the kernel).

### The blocks under the filesystem

MicroFS is a consumer of blocks, not of a flash chip: `kernel/blockdev.h`
declares a device (a sector size, a count, read and write, an optional erase
and sync) and the kernel keeps a small registry of the devices the platform
registered at the boot. On the board the device is `flash0`, the partition
`mfs` of `partitions.csv`; in the tests it is an array in the memory. The
filesystem asks for sector 17 and does not know which of them it got, which
is what lets the persistence tests run on a laptop.

The store behind `/etc` is the same idea for a handful of strings:
`kernel/port/kv.h` (get, set, erase, commit, next) is a duty of the port,
because a board has one store. The ESP32 answers with the NVS, the host with
a file per key, and the configuration does not know the difference - which
is why `config sync` can be tested without a board.

## 6. Memory (phase 3)

* `kmm_alloc`/`kmm_free` — our own allocator (bitmap or slab on top of the ESP-IDF
  heap).
* Large objects (frame buffers, file caches) live in PSRAM, small ones in internal
  SRAM (which is scarce — that is the constraint).
* Virtual memory is emulated: each task has its own "address space", but without
  an MMU a switch means switching the stack pointer and the data segment base.

## 7. Drivers (phase 4)

The interface follows Linux:

```c
struct file_operations {
    int  (*open)(struct inode *ino, struct file *f);
    ssize_t (*read)(struct file *f, void *buf, size_t count);
    ssize_t (*write)(struct file *f, const void *buf, size_t count);
    int  (*ioctl)(struct file *f, unsigned long cmd, void *arg);
};
```

The drivers are written: `drv_display` (ST7789v2, the panel and the turn of
its picture), `drv_input` (A/B/C), `drv_buzzer`, `drv_imu` (MPU6886),
`drv_rtc` (BM8563), `drv_battery` and `drv_mic` (SPM1423, a PDM microphone
on I2S at 16 kHz; the doctor reports its RMS and peak). The pinout and the
quirks are documented in
[docs/ports/esp32/hardware.md](ports/esp32/hardware.md).

The WiFi driver is built like the kernel itself: a portable core
(`components/wifi`) that owns the state of the station and a radio
(`components/port_esp32/src/wifi_radio.c`) that talks to the ESP-IDF. The two talk
through a table of functions, so the state machine of the driver is covered
by the unit tests of the host. The decision is recorded in
[ADR 0003](adr/0003-wifi-driver.md).

### The panel is one piece of hardware

The SPI of the panel is a sequence: the command, the data, the window and
the pixels have to arrive in that order, and a second task that writes its
own pixels in the middle of a transfer changes the meaning of the commands
on the wire. Everything that draws (`drv_display_clear`, `drv_display_text`,
the graphics (`drv_display_pixel`, `drv_display_fill_rect`, `drv_display_rect`,
`drv_display_line`), `drv_display_flush`, `drv_display_set_offset`,
`drv_display_set_rotation`) takes the lock of the panel, and the lock is
recursive because the functions of the panel call each other. The shared
buffer of the transfer, the offset of the image and the turn of the picture
are behind the same lock.

The console of the board (`components/graphics`) does not draw from the task
that prints. It writes the lines down and says that the screen is out of
date, and one task of its own, of a low priority, brings the screen up to
date at most ten times a second: a listing of fifty lines is one redraw of
the panel instead of fifty, and the task that prints does not stand still
in the middle of the dialogue. The layout of the dialogue is of the picture
as it is turned: 22 columns of 26 rows in the portrait, 40 of 15 in the
landscape.

The same console has a second door: `components/telnet` puts the shell on
port 23 of the network, beside the UART and the display. The client is
registered as one more sink of the console, so a command over the air
sees the same output as a person at the USB cable; the negotiation of
telnet is answered with refusals, because the shell needs no options —
the client keeps its own echo. There is no authentication: the board is
on the network of its owner, and `telnet off` closes the door.

The sockets under all of this are a duty of the port as well:
`kernel/port/net.h` offers a connection, a server, a send that may wait or
not, and a one-shot UDP exchange - and nothing else. There is no TLS, no
IPv6 and no raw socket: the board is on the network of its owner. On the
board the port speaks lwIP, on the host it speaks the sockets of the
process, so the HTTP client, the clock of NTP and the server of telnet are
exercised without a board.

## 8. Programs (phase 7)

A program is a file, not a function of the tree: `execve` reads it, the kernel
parses a format of its own and the port runs it. The decisions are in
[ADR 0005](adr/0005-programs-and-protection.md), the report of the phase in
[phase7.md](phases/phase7.md).

### The format: MOX1

An executable is a flat image with a 32-byte header (`kernel/exec.h`): the
magic `MOX1`, a version, flags, the offset of the entry, the sizes of the text,
the data and the bss, and a reserved word. The image is linked for the window
of the port - the code at its base plus the 32 bytes of the header - and copied
as it is, because nothing relocates it; the bss is not in the file and the port
clears it. `tools/mkprog.sh` turns an object into the image. The parser
(`components/kernel/src/exec.c`) checks the image against itself and nothing
else: the magic and the version, sizes that fit the file, a text that is not
empty, an entry in the text, sizes that are multiples of four, and a bss under
a megabyte.

### `execve` belongs to the port

`kernel/port/exec.h` is a subject of the contract (1.3) with three calls:
`k_port_exec_supported()` (can this machine run a program at all),
`k_port_exec_base()` (where the window is) and `k_port_exec_enter()` (copy the
image, clear the bss, jump). The kernel parses the image and asks the port, so
the answer for a machine that has no window is `ENOSYS` at the edge. The board
says no: its flash is not executable for a file, and internal SRAM is ~100 KB
for everything. The host says no too: jumping into the bytes of a program
inside the address space of the simulator would run it with the libc of the
host under it. x86-64 says yes and places a program at 32 MiB, with a 64 KiB
stack at the top of its window; its boot mounts MicroFS on a volume of the
port's RAM, writes `/bin/hello` there, gives it the mode of a program and asks
for it with `execve` - the stat, the mode, the read through the VFS, the parse
and the jump are the ones of the call.

### The rights are checked at the edge

`chmod` and `chown` are numbers 23 and 24 of the syscall table and `execve` is
25. A mode is nine bits (`rwx` for the user, the group and the others) and an
owner is a fifteen-bit uid; both live in the spare bits of the MicroFS record
(see [memory.md](memory.md)). There is one user (uid 0), and it gets no
exemption of root: `open` compares the access it was asked for with the triad
that decides for the caller, and `execve` needs the `x` bit; `stat` only
reports. The width of the two numbers is checked at the door, the library
(`components/libc`) has the three wrappers with the codes of POSIX in
`errno`, and the shell has `chmod`, `chown` and `exec` over the same calls.
The directories of a path are not checked for their `x` bit yet, and
`fork()` with an address space of its own waits for the paging of a port
(ADR 0005). A program calls the kernel with `int $0x80` on the x86-64 port:
the register ABI is in [ADR 0006](adr/0006-program-entry.md), and the program
runs in ring 0 without an address space of its own, so the handler trusts its
pointers and a program that does not return holds the machine (see
[debts.md](debts.md)).

## 9. Testing

* The kernel is tested on the host: `make test` (own framework, no dependencies),
  twice more under the address sanitizer (`make test-asan`) and the race
  detector (`make test-tsan`).
* The simulator `make sim` runs the very same kernel on Linux with a volume on
  a file, `/proc`, `/dev`, the configuration and the drivers of the host, so
  the whole portable system can be debugged without flashing.
* The x86-64 port is booted in QEMU by `make x86-smoke`: the same sources, a
  bare processor, a serial console, the shell - and, between the banner and
  the shell, a MOX1 program that the port enters and that writes its line.
* On the device there is a UART console, and its output is mirrored to the
  TFT at the same time — the dialogue and the boot report are on both; the
  checks are driven by `tools/device-test.sh`.
* `make integrity` holds the boundary: the kernel includes only the kernel, a
  portable component no platform header and no `k_port_*`, every port
  implements the whole contract, and the image fits its budget.

## 10. Resource budget

| Resource | Available | Comment |
|---|---|---|
| Flash | 8 MB | `factory` 2 MB + MicroFS 5.75 MB (one application, no OTA) |
| PSRAM | 2 MB | file data, buffers |
| Internal SRAM | ~100 KB usable by code | task stacks, drivers |
| CPU | 240 MHz, 2 cores | already taken by FreeRTOS; our kernel lives on top |
| Battery | 200 mAh | deep sleep + RTC is the main "wearable" scenario |
| Image | 2 MB `factory` slot | the firmware is 886 KB (58 % of the slot free) |
