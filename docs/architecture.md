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
| Ports and substrates:                                                |
|   port_esp32   ESP-IDF + FreeRTOS + lwIP + NVS, the partition of MFS |
|   port_host    a Linux process (pthreads, files, sockets) - the  |
|                substrate of the tests and the simulator, not a port |
|   port_x86_64  bare metal on QEMU: its own loader, IDT/PIT, switch  |
|   port_null    no machine at all, linked by the checks - a shape |
|                of the contract, not a port                          |
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
| `kernel/port/cpu.h` | the cores of the machine | `k_port_cpu_count`, `k_port_cpu_id` |
| `kernel/port/time.h` | time since the boot | `k_port_uptime_ms`, `k_port_uptime_us` |
| `kernel/port/mem.h` | internal heap and PSRAM | `k_port_alloc`, `k_port_heap_free` |
| `kernel/port/lock.h` | critical sections, recursive mutexes | `k_port_enter_critical`, `k_port_mutex_lock` |
| `kernel/port/power.h` | halt, reset | `k_port_halt`, `k_port_reset` |
| `kernel/port/kv.h` | the store that survives a restart | `k_port_kv_get`, `k_port_kv_next` |
| `kernel/port/net.h` | TCP and UDP | `k_port_net_connect`, `k_port_net_listen` |
| `kernel/port/info.h` | identity and contract version | `k_port_name`, `k_port_api_major` |
| `kernel/port/exec.h` | running a program | `k_port_exec_supported`, `k_port_exec_arch`, `k_port_exec_base`, `k_port_exec_enter` |

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

### The contracts are recorded in the tree

Two interfaces are the promise of a release, and both have a golden record
that a change has to move with the code:

| Record | What it freezes | Checked by |
|---|---|---|
| [tests/corpus/abi.txt](../tests/corpus/abi.txt) | the calls (number, name, arity, result), the error codes, the flags, the limits, the header of a program image and the layout of the structures that cross the door, on a 64-bit and on a 32-bit machine | `ksys: the ABI is the recorded one` (`tests/cases/test_ksys.c`), on every `make test` |
| [tests/corpus/port_api.txt](../tests/corpus/port_api.txt) | the 47 functions of `kernel/port/*.h` and the version of the contract | `make integrity` (`[port]`): the record against the headers, and the whole list against the ESP32 and the x86-64 port |

[tests/corpus/mox1](../tests/corpus/mox1) holds a MOX1 image of version 2,
built for 1.0 and recorded in hex: `k_exec_parse()` reads it on every
`make test`, and the format is expected to read it for as long as it is 1.x.
The golden files are not generated into the build directory on purpose - a
record that a build rewrites is not a record. A change of the ABI or of the
contract is therefore two files in one commit, and a reviewer sees it.

## 2. What is Linux-like here, and what is not

| Property | Linux | Micro OS |
|---|---|---|
| Modular monolith | yes | yes (kernel + drivers in one address space) |
| Layered subsystems | yes | yes (`kernel/boot.h`, initialisation levels) |
| Processes / threads | yes | yes, simplified, from phase 1 |
| Filesystem | yes | yes, from phase 2 (in-RAM FS plus a VFS layer) |
| `/proc`, `/dev`, `/sys` | yes | `/proc` and `/dev` from phase 2, `/sys` later |
| System calls | yes | yes, from phase 3; hardware isolation on x86-64 (ring 3), logical on the board |
| Memory protection | MMU | **on x86-64 only**: page tables per program, a user window and ring 3 (ADR 0007); the ESP32 has no MMU and keeps the logical boundary |
| Multi-user | yes | partial (a uid and a gid per task, inherited and checked; `/etc/passwd` and `/etc/shadow`, `login`, `su` and `id` on the console; no setuid bit in an image and no group in a record, ADR 0033) |
| Dynamic linker | yes | no (everything is statically linked) |

We honestly call this **Linux-like**, not Linux. On the board there is no
MMU: the separation of user and kernel space is logical, system calls are
ordinary function calls with a uniform ABI, and the boundary is kept by
discipline. The x86-64 port has an MMU and uses it: a program gets page tables
of its own, a 16 MiB window at 32 MiB, a guard page under its stack and ring
3, and the kernel is supervisor-only and out of its reach. A fault of a
program is reported and the program is taken down; the shell that ran it is
alive. A program is a task of the scheduler with a CR3 of its own, and `fork()`
clones its page tables and copies a page on the first write (ADR 0007). The
pointers of a program are checked and copied at the edge of the call: the
port says whether a range is memory of the caller, and an address that is not
mapped is `EFAULT` instead of a page fault of ring 0. The kernel is in the
higher half: the image is linked at `0xFFFFFFFF80000000`, the loader maps it
there beside the low identity map, and the C of the kernel is built with
`-mcmodel=kernel` (ADR 0007). The heap and the devices stay in the identity
map, which is what lets a driver hand a buffer to a card.

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
* A task that ends leaves its children to init: `task_exit()` reparents every
  live child to pid 1, and the init of the port reaps the zombies nobody waits
  for. Without it a program killed while it had a child left a slot of the
  table of 32 taken for the life of the machine.

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

### Many cores (ADR 0012)

The scheduler of 1.0 has one `current`, one ready queue and one idle loop,
because one processor was the whole machine. The first phase of ADR 0012 is
in the tree and the flag `KERNEL_SMP` (kernel/config.h) decides which kernel a
build is:

* **`KERNEL_SMP=0`** (the default, the kernel of 1.0): one slot of the array
  of cores, one run queue, the same code and the same tests as before.
* **`KERNEL_SMP=1`**: `struct k_cpu` per core (kernel/cpu.h) with the current
  task, the ticks of the core, the nesting of its interrupt state and the
  stack, the thread pointer and the tables the port owns. Every core has a
  run queue per priority, `struct task` carries the core that owns it, and
  only the scheduler writes that field: a task is created on the next core of
  a round robin, woken on the core it already had, claimed by the core that
  runs it, and moved by `k_sched_migrate()` - never while it runs.

The lock of the queues is the critical section of the port (saved flags,
interrupts off, a lock of the machine on a port that has a second core) with
the kernel's own nesting count, reached through `k_cpu_lock()`. It is held
for the bookkeeping of a queue and dropped before the tick charges a task,
before a sleep and before a switch, so the second core is not stopped by a
task that waits for the filesystem. The kmm, the VFS, the console and the
block layer keep their own locks (`k_mutex`, a sleeping lock that belongs to
a task; the kmm's owner-and-depth lock, which sleeps a millisecond between
tries); the audit of what a second core changes for them is the debt K8 of
[debts.md](debts.md), not a silence.

The price of the shape is static memory: the run queue of a core is
`SCHED_READY_CAPACITY` slots of a pointer for each of the sixteen priority
levels, about 4 KiB, so a build of four cores carries about 16 KiB of `bss`
and the one-core build carries the array it always had.

Each core has its own count of ticks; the millisecond of the kernel is
advanced by the tick of the core 0, where the clock of every port of the tree
lives (the PIT of x86-64, the one thread of the host). A port whose timer is
on another core says so where it starts the clock.

The port answers how many cores it started (`k_port_cpu_count`) and which one
is asking (`k_port_cpu_id`). The x86-64 port reads the processors from the
MADT of ACPI, starts the application processors and counts only the ones that
answered (ADR 0012, "The pilot of 1.x, closed"): the boot line is
`2 cpu(s) reported by the firmware, 2 in use`, and a core that did not answer
is named and left out. The host port takes the number from `MICRO_OS_CPUS`
(one by default, two in `make test-smp`, `make test-tsan` and
`make soak-host`), and every task there is a pthread, so a "core" is a queue
of the kernel and an identity of the task. A port that predates the two calls
links against their weak default (one core) and keeps working.

The claim is the one call that makes the choice of a task safe under two
cores: `k_sched_claim_next()` chooses the task and marks it RUNNING on the
calling core in the same critical section, so the window between the choice
and the jump of the switch is closed. A core picks only from its own queue: a
task that has just used up its quantum is READY in that queue for a few
instructions before the switch, and a second core that took it would resume a
stack that is still in use. A task changes core at creation, on wake after a
migration, and through `k_sched_migrate()`.

Every core also has a context that is not a task: `kmain()` on the core 0 and
the idle loop of an application processor. It is what the port returns to
when the last task of a core ends, and it runs `schedule()` in a loop, so the
next task of that core is always claimed. The port's current context, its
pending stack and address space, and its thread pointer are per core
(`struct port_cpu`, ports/port_x86_64/src/port_x86_64_task.c), and a core
names itself by the first word of a record behind `IA32_GS_BASE`.

The console is one resource of the machine and `k_port_write()` holds the big
kernel lock for the whole call. The heap of the port and the pool of the page
tables are sections of the machine as well: the heap takes the interrupts of
the core with the lock, because a tick that preempted its holder could switch
to a task that allocates, and the pool of the tables is shared by every
address space. `CR0.WP` is set on every core, because it is a register of the
core and the kernel's copy into a read-only page of a program has to fault
there too. The timer and the keyboard are entries of the I/O APIC of the
firmware (ADR 0034): one redirection entry per line, the destination core in
the entry, the 8259 masked as a whole, and the EOI of a line goes to the local
APIC of the core that took it. A machine whose firmware describes no chip
keeps the 8259, which delivers everything to the core 0, whose LINT0 is the
only one in the external mode. The APs take their ticks from their own local
APIC timer (1 kHz, calibrated against the TSC), and a task put in the queue of
another core wakes that core with an inter-processor interrupt instead of
waiting for its tick. The open work is K8 of [debts.md](debts.md).

### Signals (ADR 0015)

A signal is a bit of `struct task` (`sig_pending`) and a disposition in a
small table of the task, not a message on a queue: the sender - `kill(pid,
sig)` - sets the bit with one atomic operation and wakes the task, and the
receiver answers it in its own context. Every access to the bit and to the
table is an atomic, because the critical section of a port is the interrupts
of one core and not a lock of the machine (ADR 0012). The points of delivery are the way
out of a system call (`ksys_call()`) and the tick of the timer when the
interrupted code is a program of ring 3. The default action of `SIGINT`,
`SIGTERM` and `SIGPIPE` is the end of the task with the status `128 + sig`;
`SIGCHLD` is ignored by default and is marked on the parent by `task_exit()`.
An installed handler is a function of the program: on x86-64 the port builds
a frame below the stack pointer of the program, writes the handler and the
signal number into the saved frame, and the `iretq` of the stub enters it;
the return address is a page of the port that calls `sigreturn`, which puts
the interrupted frame back. The host, the board and the null port have no
ring 3 and run the handler in place. `waitpid()` looks at a finished child
before the marks and answers `EINTR` for a mark that has to be answered
without reaping the child. There is no job control - no groups, no `&`, no
`fg`, no `bg` - and the documentation says so; Ctrl+C while the shell waits
for a program is read by the port and becomes `SIGINT` for that program.

### The limits of a task (0.9)

A task has three limits - the memory it holds, the processor time it spends
and the descriptors it opens - and the calls `getlimit` (32) and `setlimit`
(33) read and lower them. The limits are fields of `struct task`, so a fork
inherits them and the scheduler does not have a second table to keep in step.
They start at the ceiling of the machine (2 MiB of PSRAM, about 49 days of
CPU, sixteen descriptors) and only go down: the ceiling belongs to the
machine, and neither a task nor root lifts it.

The memory is counted by the allocator: `kmm` asks `task_memory_fits()` before
it looks for a block, charges the user blocks to the task that allocated them
and gives the charge back on `free`, so a task that is over its budget does
not spend the memory of the system on the way to `ENOMEM`. The kernel and the
system priorities have no owner and no limit. The CPU time is the time
between the points where the task is known to be running - the edge of a
system call and the tick of a port that can name the running task - with the
sleep on both sides of a wait set aside; the limit marks the task with
`SIGTERM`, the closest signal of the four to `SIGXCPU`, and the default action
ends it with 143. The descriptors stop at the limit in `kfile_alloc()`,
`kfile_pipe()` and `kfile_dup2()` with `EMFILE`, while a full table of the
system is `ENOSPC`; lowering the limit never closes what is already open.
`ulimit` of the shell prints the three numbers and lowers them, and the
library has `microos_getlimit()`/`microos_setlimit()`.

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

* every open file is a description of the kernel, counted by the descriptors
  that refer to it (`kernel/kfile.h`): the position and the flags of the file
  live there, not in a table of the filesystem, and `dup2` or a fork gives a
  second descriptor the same open file. Two tasks that read their own files
  never touch each other's position;
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

### The pipes

A pipe is a ring of the port - 64 KiB on x86-64 and the host, 4 KiB on the
board - in a table of sixteen entries of the kernel, with the counts of the
reading and the writing descriptions over it (ADR 0035, which changes the
ring and the wait of ADR 0014). The ring is not a block in the bss of the
kernel: it comes from `kmm_alloc` with the pipe and goes back with its last
end, so sixteen pipes of 64 KiB cost a megabyte only when they are all open.
The two ends are ordinary descriptors of the task, so the access check, the
close, `dup2` and the copy of a fork need no special case; a write with no
reader is `EPIPE`, because the kernel has no signals yet, and a write of at
most `PIPE_BUF` bytes is one step of the ring. A wait of a pipe is a record of
the task on the channel of the pipes (`kernel/wait.h`, ADR 0046): it is armed
under the lock of the table, before that lock is dropped, so the writer cannot
put a byte in between and lose its wakeup; the writer then takes the waiter
out of the sleeping table in the call of the write, and the wait has no bound
- a reader slower than its writer is an ordinary pipeline, and a signal cuts
the wait short with `EINTR`. The waiters are a table of the size of the table
of the tasks and the wake is a broadcast of the channel, which is coarse on
purpose: a waiter that finds itself woken and has nothing to read looks again.

### The blocks under the filesystem

MicroFS is a consumer of blocks, not of a flash chip: `kernel/blockdev.h`
declares a device (a sector size, a count, read and write, an optional erase
and sync) and the kernel keeps a small registry of the devices the platform
registered at the boot. On the board the device is `flash0`, the partition
`mfs` of `partitions.csv`; in the tests it is an array in the memory. The
filesystem asks for sector 17 and does not know which of them it got, which
is what lets the persistence tests run on a laptop.

The data of a file is a chain of blocks: the first four bytes of every block
name the next one and the record names the head. A chain is a list with no
table of the places, so the n-th block is reached by the walk through the n
before it, one read of the device for each step. The descriptor of an open
file therefore keeps the block it read last (`struct mfs_file` in
`mfs/mfs.h`): a read goes on from there, a block costs one step, and only a
seek back to an earlier block starts at the head again. Without that cursor
a sequential read walked from the head for every block and a file of half a
megabyte cost the square of its blocks - seconds between `exec` of a large
program and its first line, which the port of zstd found.

The store behind `/etc` is the same idea for a handful of strings:
`kernel/port/kv.h` (get, set, erase, commit, next) is a duty of the port,
because a board has one store. The ESP32 answers with the NVS, the host with
a file per key, and the configuration does not know the difference - which
is why `config sync` can be tested without a board.

### The record, its moments, and `statx`

A record of MicroFS of today is 64 bytes: the type, the first block, the
size and the count of entries, `mtime` and `ctime` as the seconds of Unix in
64 bits, the group of the object and a CRC32 of the record (ADR 0038). A
volume of version 2 keeps the 32 bytes of the releases before the group and
the checksum, and a volume of version 1 the 16 bytes before the times; both
are mounted without a rewrite, their records report zero for what they have
no room for - the moments of a version 1 record are "unknown" and not 1970,
and a volume without a group has the group of root. The version in the
header and the bits of `features` (`TIME`, `RECORD_CRC`) are read together,
so a header that claims the room without the feature (or the other way
around) is refused. The design and the migration are ADR 0013 and ADR 0038;
the layout is `docs/memory.md`.

`META` is the map of the free blocks and the table of the records, sized by
the number of the records and not by a share of the volume: the table grows
into the data section when the reserve of a new volume runs out, and a
record that a power cut left half written is one whose CRC32 does not match,
which the check names and the repair gives back (ADR 0038).

The moments come from `k_clock_now_unix()`, the clock of the kernel that the
RTC, NTP and the host process feed. A machine that was never set falls back
to the seconds since its boot, so a stamp always orders the changes even
when it is not a date. The filesystem never calls a library of the caller
and the number does not travel through a program.

`struct vfs_stat` and `KSYS_STAT` are frozen, so the moments have a call of
their own: `KSYS_STATX` (26) copies a `struct vfs_statx`, the five fields of
`stat`, the group and the two times. The VFS operation is optional: procfs,
devfs and the FAT reader answer through their `stat` and leave the group and
the times zero. The library exposes `microos_statx()` and `struct
microos_statx`. The structure grew by the word of the group in 1.7 (ADR
0038), so the call copies 48 bytes and the two headers of the tree move
together.

### The types of filesystems

The VFS knows no name of a filesystem. It keeps a table of types
(`struct vfs_fs_type`: a name and the call that mounts a volume), and
`vfs_mount_type(point, "fat32", "ata0p1")` finds the call and runs it. The
call mounts the volume itself with `vfs_mount()`, so the resolution of a
path, the search bit and the listing of the mount points are the same for
every type. A mount function may be a wrapper of a format of the board
(MicroFS) or a reader of a format of the world (FAT, ext2); the VFS does not
care which.

The types are registered by the image, not by the kernel: a build that does
not link the reader never has its name, and a boot that asks for an unknown
type gets `KERR_INVAL`. MicroFS is still mounted at the root by
`mfs_vfs_mount()` as it always was; the registry is for the volumes below
`/mnt` and for the filesystems that arrive later. The option of the build is
`MICRO_OS_FS_FAT32` (off by default on the board, compiled into the host
tests), the design is ADR 0009.

The FAT reader is a few files under `components/fs_fat32/`: the boot sector
and the table of clusters, the directories with their long UTF-16 names and
the adapter to the VFS. It is **read and written**; an object of FAT has no
mode and no owner (the numbers are zero), `chmod` and `chown` are
`KERR_NOTSUP`, not `KERR_PERM`, and a write without a journal is what a
power cut can catch. A FAT volume mounted at `/mnt/fat` is one
more entry of the mount table, and `ls /mnt` shows it even though MicroFS
does not have the directory.

The ext2 reader under `components/fs_ext2/` is the first one with inodes:
the superblock, the group descriptors, the inode table, the direct blocks
and the one, two and three levels of indirection, and directories whose
names the entry itself holds. It is **read-only**, and that is why it can
report what FAT cannot: the nine bits of `i_mode` as the mode, `i_uid` as
the owner and `i_mtime`/`i_ctime` through `statx`. A write, a new name, a
mode and an owner are `KERR_NOTSUP`, and a journal of ext3 is not replayed,
so a volume with one is refused. ADR 0019 is the design.

### Disks and partitions

A disk is a block device too, and a partition of it is one: the portable
parser of `components/kernel/src/partition.c` reads the MBR or the GPT in the
first sectors, holds every entry against the size of the disk and registers a
child device with the name of the parent and `p` plus the number of the entry
(`ata0p1`, `virtio0p1`). A child adds the first sector of its partition to
every request and refuses what runs past its end; the filesystem mounted on it
does not know that a table exists.

Three rules of the layer are worth remembering: the parser never writes to
the medium (a broken table is a refusal, not a repair); a disk without a table
is not a volume, there is no "the whole disk is the filesystem" special case;
and the drivers live in the ports (the ATA channel and the virtio-blk device
of x86-64, SDHCI on the board) while the tables live in the kernel, because a
table is a property of the medium and not of the controller. ADR 0008 is the
design of the layer, ADR 0032 the physical address of a buffer a device reads
and the bus-master path of the IDE channel, and ADR 0037 the second disk: the
same image on a queue of virtio descriptors, a request as long as the queue
allows and the interrupt of a PCI line.


## 6. Memory (phase 3)

* `kmm_alloc`/`kmm_free` — our own allocator (bitmap or slab on top of the ESP-IDF
  heap).
* Large objects (frame buffers, file caches) live in PSRAM, small ones in internal
  SRAM (which is scarce — that is the constraint).
* Virtual memory is emulated on the board: each task has its own "address
  space", but without an MMU a switch means switching the stack pointer and the
  data segment base. The x86-64 port gives a program real page tables and a
  `fork()` that copies them with copy-on-write (ADR 0007); the kernel of that
  port is linked in the higher half and mapped at the top, and the low identity
  map is kept for the heap, the arena and the devices (ADR 0007, "The higher
  half: taken").

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
`drv_rtc` (BM8563), `drv_battery`, `drv_mic` (SPM1423, a PDM microphone on
I2S at 16 kHz; the doctor reports its RMS and peak), the sound detector on top
of it, `drv_ir` (NEC on the pin of the red LED, with the arbitration of the
pad between the two) and `drv_power` (deep sleep, `HOLD` and the wake of the
timer, of BUTTON C and of the alarm of the clock through the timer of the
chip). The pinout and the quirks are documented in
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
IPv6 and no raw socket in that contract: the board is on the network of its
owner. TLS lives above the contract and not in it: `components/net` wraps a
transport of bytes with mbedTLS on the machines that have the library
(x86-64 and the host tests), and an image without it answers that `https`
is not in it (ADR 0024 §3). On the board the port speaks lwIP, on the host
it speaks the sockets of the process, so the HTTP client, the clock of NTP
and the server of telnet are exercised without a board.

## 8. Programs (phase 7)

A program is a file, not a function of the tree: `execve` reads it, the kernel
parses a format of its own and the port runs it. The decisions are in
[ADR 0005](adr/0005-programs-and-protection.md), the report of the phase in
[phase7.md](phases/phase7.md).

### The format: MOX1

An executable is a flat image with a 32-byte header (`kernel/exec.h`): the
magic `MOX1`, a version (2), flags, the offset of the entry, the sizes of the
text, the data and the bss, and the machine the bytes are code for (`arch`: 0
x86-64, 1 Xtensa). The image is linked for the window of the port - the code
at its base plus the 32 bytes of the header - and copied as it is, because
nothing relocates it; the bss is not in the file and the port clears it.
`tools/mkprog.sh` turns an object into the image and writes the machine. The
parser (`components/kernel/src/exec.c`) checks the image against itself and
nothing else: the magic and the version, an arch the format knows, sizes that
fit the file, a text that is not empty, an entry in the text, sizes that are
multiples of four, and a bss under a megabyte. The machine of the image
against the machine of the port is the question of `k_exec_run`, which asks
the port (`k_port_exec_arch`) before it enters a byte.

### `execve` belongs to the port

`kernel/port/exec.h` is a subject of the contract (1.8) with eight calls:
`k_port_exec_supported()` (can this machine run a program at all),
`k_port_exec_arch()` (which machine of the format it is, or -1),
`k_port_exec_base()` (where the window is), `k_port_exec_enter()` (place the
image, enter it), the two halves of the end of a program,
`k_port_exec_active()` and `k_port_exec_exit()`, the edge of the memory of a
program, `k_port_exec_contains()`, and the copy of a context for `fork()`,
`k_port_fork_copy()`. The kernel parses the image and asks the port, so the
answer for a machine that has no window is `ENOSYS` at the edge, and an image
of another machine is `ENOEXEC` before the port is asked to enter it. The
board says no to a program today: its flash is not executable for a file, and
internal SRAM is ~100 KB for everything (ADR 0018 gives it a partition and a
loader; the port has the machine, not the loader). The host says no too: jumping into
the bytes of a program inside the address space of the simulator would run it
with the libc of the host under it. x86-64 says yes: it builds page tables for
the program, maps the window at 32 MiB onto a physical arena at 64 MiB, keeps
a 64 KiB stack at the top of the window with an unmapped guard page below it
and enters ring 3 through an `iretq`. Its boot mounts MicroFS on a volume of
the port's RAM, writes eleven images to `/bin` - `hello`, `crash`, `fork`,
`badptr`, `signals`, `nx`, `cat`, `echo`, `ls`, `spawn` and `wc` - gives them the
mode of a program
and asks for `/bin/hello` with `execve` - the stat, the mode, the read through the VFS,
the parse and the transition are the ones of the call. A program ends with
`exit` and the task it ran in becomes a zombie for its parent: the caller of
`execve` without an address space (the shell, the boot) waits for it and gets
0, and a program that replaced itself is reaped by the `waitpid` of its own
parent, which gets the status the program ended with. A fault of ring 3
prints `the program faulted: vector N at 0x...` and ends the task with status
255 (ADR 0007).

### The entry and the library of a program

The port enters a program with `argc` in `rdi`, `argv` in `rsi` and `envp` in
`rdx`: the arguments of a C function, on a stack aligned as after a call whose
top word is zero (ADR 0016). `execve(path, argv, envp)` copies both vectors at
the edge - at most eight words and 512 bytes each - and a NULL environment is
the one of the system (`k_exec_environ()`), so every program of the machine
starts with the same shape. The environment travels to the port in
`struct k_exec_image.envp`; the contract stays 1.6.

The library of the kernel is linked into the kernel and calls `ksys_*`, which
a program of ring 3 cannot reach, so a program is linked against
`components/libc/user`: `_start`, the door of `syscall`, `open`, `close`,
`read`, `write`, `lseek`, `readdir`, `getpid`, `uptime`, `exit`, `printf`,
`puts` and five functions of strings. The numbers of the calls come from
`kernel/ksys.h`, the same header the kernel uses. The image carries its copy:
there is no dynamic linker.

### The rights are checked at the edge

`chmod` and `chown` are numbers 23 and 24 of the syscall table and `execve` is
25. A mode is nine bits (`rwx` for the user, the group and the others) and an
owner is a fifteen-bit uid; both live in the spare bits of the MicroFS record
(see [memory.md](memory.md)). Every task has a uid and a gid (calls 36 to 40)
and takes them from the task that made it. `open` compares the access it was
asked for with the triad that decides for the caller and `execve` needs the
`x` bit; root passes the read and the write, and no filesystem stores a group,
so the triad of the others is the one a task that is not the owner reads.
`chmod` is the owner or root, `chown`, `mount` and `umount` are root, and a
signal to a task of another uid is `EPERM`; `stat` only reports. The width of
the two numbers is checked at the door, the library (`components/libc`) has
the three wrappers with the codes of POSIX in `errno`, and the shell has
`chmod`, `chown` and `exec` over the same calls. The directories of a path are
checked for their `x` bit, and `fork()` with an address space of its own
still waits for the copy of the page tables and copy on write (ADR 0005,
ADR 0007). A program calls the kernel with `syscall` on the x86-64 port; the
vector 0x80 is the second door of the same convention, the one a program of
another compiler and the restorer of a signal handler use, and the return of
either instruction is the frame of the same 160 bytes. The register ABI is in
[ADR 0006](adr/0006-program-entry.md), the program runs in ring 3 with page
tables of its own, and the processor switches to a kernel stack of the program
through the TSS on the way in. The pointers of the caller are checked at the
edge and copied there (`k_port_exec_contains`, `copy_from_user`): a range
that is not memory of the program is `EFAULT`, not a fault of ring 0, and the
strings of a path or a vector never reach the VFS (ADR 0007).

## 9. Testing

* The kernel is tested on the host: `make test` (own framework, no dependencies),
  twice more under the address sanitizer (`make test-asan`) and the race
  detector (`make test-tsan`).
* The simulator `make sim` runs the very same kernel on Linux with a volume on
  a file, `/proc`, `/dev`, the configuration and the drivers of the host, so
  the whole portable system can be debugged without flashing. It links the
  network of `components/net`, and `make smoke` fetches a page of its own
  process over the loopback with the HTTP client the board runs.
* `make coverage` builds the host with `--coverage`, runs the tests and the
  simulator, and gcov counts the lines of `components/kernel`; the gate is 70%
  of lines (the tree met about 80.7% when it was added).
* `make reproducible` builds the disk image twice with `SOURCE_DATE_EPOCH` and
  compares the bytes. The same moment fixes the records of MicroFS, the build
  id and the paths of the sources; the cloud does the same for the firmware.
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
| Flash | 8 MB | two 1.125 MiB application slots + 132 KiB of programs + MicroFS 5.56 MiB (ADR 0022) |
| PSRAM | 2 MB | file data, buffers |
| Internal SRAM | ~100 KB usable by code | task stacks, drivers |
| CPU | 240 MHz, 2 cores | already taken by FreeRTOS; our kernel lives on top |
| Battery | 200 mAh | deep sleep + RTC is the main "wearable" scenario |
| Image | 2 MB `factory` slot | the firmware is 886 KB (58 % of the slot free) |
