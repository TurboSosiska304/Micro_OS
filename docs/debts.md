# Debts of Micro OS

This is the single register of what the tree knows it does not do. A debt is a
gap between what a document promises and what the code does, or a limitation
with a next step that somebody can take. A decision is not a debt: the choices
that are final are listed in their own section, so that a reader does not try
to "fix" them.

The sources of the register are [roadmap.md](roadmap.md) (K3-K7),
[phase7.md](phases/phase7.md) ("What is still missing"),
[ports/esp32/README.md](ports/esp32/README.md) and
[ports/esp32/roadmap.md](ports/esp32/roadmap.md) (P3-P5), the other port
READMEs, [ADR 0005](adr/0005-programs-and-protection.md),
[README.md](../README.md) ("Known limitations"), [libc.md](libc.md),
[memory.md](memory.md) and [syscall-abi.md](syscall-abi.md).

| Status | Meaning |
|---|---|
| `open` | Known, not started; the next step is written in the last column. |
| `in work` | Somebody is changing the tree for it now. |
| `decision` | Deliberate, not a debt; the reason is in "Decisions, not debts". |

A debt is closed by code and a test, not by deleting its row: when it is done
the row moves to the release notes of the phase that did it.

## K3 - Programs

| Debt | Where | Status | What it needs |
|---|---|---|---|
| `stat` reports the mode and the owner to anybody and checks nothing. | `components/kernel/src/ksys.c`, `docs/phases/phase7.md` | decision | None: POSIX makes `stat` readable, and the check stays at `open` and `execve`. |
| `ESRCH` and `EEXIST` share a number: `-3` became "file exists" before signals, and `kill()` of a pid that names no task answers the same `-3` (`KERR_SRCH` is an alias of `KERR_EXIST`). Renaming the old code would break the frozen table; `waitpid` still answers `ECHILD`. | `components/kernel/include/kernel/errno.h`, ADR 0015 | decision | None: the shell prints the sentence of the signal, and a program reads `KERR_SRCH` for the number. |
| The library does not know the newest codes or the directory flag: `microos_strerror` says `unknown error` for `EBUSY`, and `map_flags` does not pass `O_DIRECTORY`. | `components/libc/src/unistd.c` | open | The name of the code and the flag in the library, with a test of `open` of a directory. |
| The `x` bit of the directories of a path is not walked, so a directory of mode `0600` still lets its files be opened by their full path. | `components/mfs/src/mfs_vfs.c`, the VFS lookup | open | Check every directory on the way down, with its own tests. |
| The image is read whole into one block of `kmm` (up to `KSYS_MAX_IO`, 64 KB) before the port copies it to the window: two copies in RAM at once. | `components/kernel/src/ksys.c` (`sys_execve`), `components/kernel/src/exec.c` | open | Stream or map the image, or cap it against the memory of the machine and not only against the transfer edge. |
| The header of an image names the machine (`arch` 1 is Xtensa, ADR 0018) and the ESP32 port runs one, but the windows are fixed addresses and one program at a time: a program of the board that calls `execve` would overwrite the text it is running from, and the port answers `ENOMEM` for the second. | `components/port_esp32/src/port_exec.c`, ADR 0018 §3 | open | A pool of windows (two, or a window per task) or a loader that copies the program out of the window of the caller. |
| A program of the board shares the address space of the kernel: a bad pointer that reaches a call of the kernel (or a fault in the program's own text) is a `Guru Meditation`/panic and a reboot, not the end of one task. Measured with `stat((char *)0xFFFFFFF0, ...)` from `/bin/hello`: `LoadProhibited`, `EXCVADDR 0xFFFFFFF0`, a backtrace with the frames at `0x4009Exxx`, then `Rebooting...`. | `components/port_esp32/src/port_exec.c`, ADR 0018 §4 | open | An exception handler of the port that knows the windows of the program and ends its task where the fault is in them; an MMU (not this board) for the rest. |
| A program of the board is a file of the `progs` partition, mounted read-only at `/bin`; the directory and the 64 KiB slots are in ADR 0022, the image is built by `tools/progs_image.py`, and no image of a program is embedded in the firmware. The loader copies the bytes into the windows; the mapping of flash as instructions (`esp_partition_mmap` with a fixed virtual address) is a later loader. | `components/port_esp32/src/progs_vfs.c`, `progs_format.c`, `tools/progs_image.py`, ADR 0018 §2, ADR 0022 | done | Nothing for 1.1; the mapping is a debt of its own, below. |
| The programs of `progs` are read with `esp_partition_read()` and copied into the IRAM/DRAM windows; the flash is not mapped as instructions, so a program costs a copy in RAM for the time of the load. | `components/port_esp32/src/progs_vfs.c`, `port_exec.c`, ADR 0022 §4 | decision | None for 1.1: the copy is the path 1.0 measured and the mapping needs a fixed virtual address reserved by the first `esp_partition_mmap()` of the boot and a program linked for it. |
| The windows of a program of the board are 8 KiB of text and 8 KiB of data; a larger image is `ENOMEM`, and the compiler flags the text arena when the kernel grows into it. The stack of the program is the 8 KiB stack of a kernel task, shared with the kernel calls it makes. | `components/port_esp32/src/port_exec.c`, `exec_windows.ld`, `programs/program_esp32.ld` | decision | None for 1.0: `hello` and `cat` use about 2 KiB of text and 300 bytes of data. A bigger arena costs IRAM the kernel does not have. |
| The programs of `/bin` are written into a MicroFS volume of the raw image by the host tool, and the boot mounts that volume. A FAT volume of the same machine is mounted and writable, but no tool puts a program on it and the boot does not look for `/bin` there. | `tools/mkfs_mfs.c`, `ports/port_x86_64/src/kmain.c`, ADR 0008 | open | An installer that writes a program to the filesystem of the person's choice, and a `PATH` (or a mount of the image) that reaches it. |
| The environment of a program is the four words of the kernel (`k_exec_environ`): the shell has no `export`, so a program cannot be given `PATH`, `HOME` or a variable of the line. | `components/kernel/src/exec.c`, `components/kshell` | open | An environment of the shell, `set`/`export`, and the vector of the shell passed to `execve` instead of NULL. |
| The arguments and the environment of a call are bounded by eight words and 512 bytes each, a number of the frame of `execve`, not of the machine. | `components/kernel/src/ksys.c` (`KSYS_ARGV_MAX`, `KSYS_ARGV_BYTES`), `ports/port_x86_64/src/port_exec.c`, `components/port_esp32/src/port_exec.c` | open | A larger frame, or a vector copied into a block of `kmm` and freed after the program ends. |
| A program inherits the table of its caller now: `k_port_exec_enter()` makes the program with `task_fork_shared()`, so `cat < file`, a pipe end at 0 and `ls /bin | wc -l` reach it (ADR 0017). What is still missing is a descriptor 0 of the console for a program at the prompt: `cat` with no name and no redirect reads a descriptor that the shell did not open, and the answer is `EBADF`. | `components/kernel/src/ksys.c`, `ports/port_x86_64/src/port_exec.c` | partial | A terminal input driver (or a descriptor of `/dev/tty0`) and a shell that gives a program descriptor 0 of the console. |
| The library of a program is a second library, not a mode of the first: `components/libc` (of the kernel) and `components/libc/user` share the header of the ABI and nothing else, and the second has no allocation, no `errno`, no streams and no floating point. | `components/libc/user` | decision | None for the size of the machine: a program of the board is a command of the kernel, and the programs of x86-64 are the ones a static image can carry. |

Closed in the same pass: the `int $0x80` entry and its ABI, `argv` through
`k_exec_run()`, `PATH` and `stat` in the shell, the test of the padding of
`mkprog.sh`, its check of the linked address, the walk of the x bit, the
directory descriptor for `readdir`, `ENAMETOOLONG`/`ECHILD`, and the entry
from ring 3 with its kernel stack, the fault handler, `exit` and the errno
of a task (K5). The row that stays above is the part of the entry that waits for
K5.

## K4 - Protection

| Debt | Where | Status | What it needs |
|---|---|---|---|
| `fork()` on a machine without an MMU is still a placeholder: the child shares the memory of the parent and re-enters its program. On x86-64 the call is real - an address space per task, copy on write and `k_port_fork_copy()` (contract 1.5) - and the board keeps the placeholder because it has no MMU. | `components/kernel/src/ksys.c`, ADR 0005, ADR 0007 | done | An MMU on the board, or the same placeholder for as long as it has none. |
| `KERNEL_NOMMU` is still 1 everywhere: the x86-64 port protects a program of ring 3, but the generic kernel does not use the MMU for a task and the board has none. | `components/kernel/include/kernel/config.h`, `ports/port_x86_64` | open | Page tables per task and the flag turned off for the ports that have an MMU. |
| The owner is stored and checked but there is only one user, uid 0, so groups and a second party are not part of the model. | ADR 0005, `components/kernel/src/ksys.c` | decision | None until a second user exists. |
| The owner of the system gets no exemption of root. | ADR 0005, the mode check | decision | None: this is the rule, not a missing feature. |
| `chmod 000` cannot be expressed: a zero mode means "the record predates the modes" and is refused instead of stored. | `docs/memory.md` section 6, `components/mfs/src/mfs.c` | decision | None without a format change; a real zero needs a bit that says "the mode is set". |
| The memory limit counts the user blocks a task allocated and has not freed, but the kernel keeps no list of the blocks of a task: a block that outlives the task that allocated it is not charged to anybody, and a task that exits with blocks still held leaves them to the system. | `components/kernel/src/kmm.c`, `components/kernel/src/task.c` | open | A list of the blocks in `struct task`, walked at the exit and by a check of the limit, or a header that names the owner strongly enough to free at the end. |
| The CPU limit is accounted at the edge of a system call and at the tick of a port that can name the running task: a task of the host or of the board that never makes a call is only marked at its next call. A program of ring 3 on x86-64 is marked by the tick. The host has no preemption of its own, so the time between two calls is wall time and includes the time the process gave to another task. | `components/kernel/src/{task,sched,ksys}.c`, `ports/port_host` | open | A tick that can name the running task on every port (the thread pointer of the board), or a preemption point that charges the time. |
| A limit only goes down: `setlimit` answers `EPERM` to a raise, even to the value it had a moment ago. A shell that lowered `ulimit -n 2` cannot come back to four without a reboot. | `components/kernel/src/task.c`, `docs/syscall-abi.md` | decision | None: one user, no privilege of root, and a program must not lift its own ceiling. A second limit (a hard one, as in POSIX) would be the way to grant a raise. |

## K5 - A processor with an MMU, and the x86-64 port

| Debt | Where | Status | What it needs |
|---|---|---|---|
| An address space per *task*: the page tables are copied for a fork and the pages are copy-on-write, and a program is a task with a CR3 of its own (K4, ADR 0007). | `ports/port_x86_64/src/paging.c`, `ports/port_x86_64/src/port_exec.c` | done | Nothing on x86-64. |
| A tick could take the CPU away from a task between `k_sched_task_terminated()` and `k_port_task_exit()`: the context was in no run queue, so its stack and the page tables of its program were never released, and every program of the machine leaked twelve table pages - the pool of 256 was exhausted after about twenty programs, which the ten utilities of `/bin` made visible. The tick now keeps a zombie on the CPU until its own exit has begun, and the `exiting` flag of the context closes the window. | `ports/port_x86_64/src/port_x86_64_task.c` | done | Nothing on x86-64; a port whose scheduler marks a task dead before its own exit must keep the task on the CPU for those instructions. |
| The pointers of a program are checked and copied at the edge: `k_port_exec_contains()` (the call of contract 1.7) walks the page tables, the paths and `argv` are copied into the kernel, and a bad range is `EFAULT` (`/bin/badptr` in `make x86-smoke`). There is no SMAP, so the check and the copy use the same page tables. | `components/kernel/src/ksys.c`, `ports/port_x86_64/src/port_exec.c`, ADR 0007 | done | SMAP/SMEP is the hardware half of the rule and stays a debt of the port. |
| NX and W^X: `EFER.NXE` from the loader, text RX, rodata RO NX, data/bss/stacks/heap/arena/VGA RW NX, program text RX with data and stack RW NX, and the port audits its tables at the boot and before every program. `/bin/nx` fetches its data (`0x15`) and writes its text (`0x7`) in `make x86-smoke` (ADR 0007, section 9). | `ports/port_x86_64/src/paging.c`, `src/port_exec.c`, `src/kmain.c`, `ports/port_x86_64/programs/nx.c`, ADR 0007 | done | Nothing for a machine with NX; the port refuses an image whose text does not end on a page. |
| A fork child came back from the call of the parent with the callee-saved registers of the kernel: the syscall stub saved ten registers and the child's return path restored only those, so R12-R15 and RBX held kernel stack addresses. The stub now saves and restores all fifteen, the interrupt stub keeps the same frame shape, and `/bin/nx` found it. | `ports/port_x86_64/src/interrupts.S`, `src/port_exec.c`, `include/port_x86_64_task.h` | done | Nothing on x86-64; a port whose fork child starts from a copied frame has to copy every register the child will use. |
| No SMEP and no SMAP. Both are a documented decision for 1.0 (ADR 0007, section 10): QEMU's default `qemu64` does not advertise either, and SMAP needs `STAC`/`CLAC` around every copy of the portable kernel (`components/kernel/src/ksys.c`), which is a change of the ABI. NX is on. | `ports/port_x86_64/src/paging.c`, `components/kernel/src/ksys.c`, ADR 0007 | open | A CPUID check and two behaviours of the map (SMEP), or the `STAC`/`CLAC` window at the copy edge of the kernel (SMAP). |
| The kernel is in the higher half: the image is linked at `0xFFFFFFFF80000000` and the loader copies it to 16 MiB and maps the same bytes at the top, the C of the kernel is built with `-mcmodel=kernel`, `page_phys()` is `virtual - KERNEL_BASE + KERNEL_PHYS`, and the GDT, the TSS, the IDT, the stacks and the page tables are high. A program's space names the kernel's high half with no user bit, the W^X audit walks both halves, and `make x86-smoke` reads `_start64` and `__kernel_end` out of the ELF (ADR 0007, "The higher half: taken"). | `ports/port_x86_64/linker64.ld`, `loader32.S`, `src/paging.c`, `src/boot64.S`, ADR 0007 | done | The heap, the arena and the devices stay in the low identity map, which is what lets a driver hand a buffer to a card; the image has a 16 MiB allowance before the boot refuses a larger one. |
| No guard page under the stacks of *kernel tasks* (the stack of a program has one): a kernel stack that overflows walks into the heap of the port. | `ports/port_x86_64/src/port_x86_64_task.c` | open | An unmapped page at the bottom of every kernel stack, and the page tables of a task to put it in. |
| No IST for the machine check and the NMI: only the double fault has a stack of its own (`IST1`), so a fault in those handlers can still become a triple fault. | `ports/port_x86_64/src/interrupts.S`, `ports/port_x86_64/src/gdt.c` | open | `IST2` and `IST3` in the TSS and the gates that name them. |
| Without a disk the volume of x86-64 is still RAM (512 KB) and does not survive a reset; with one it is the first partition of a raw image on the primary ATA channel (PIO, MBR/GPT only, no DMA, no second disk). | `ports/port_x86_64/src/blockdev_ram.c`, `ports/port_x86_64/src/ata.c`, `ports/port_x86_64/src/kmain.c` | partial | A second medium and a driver with DMA or interrupts; the disk itself is written (ADR 0008). |
| The network of x86-64 has a client and a server of TCP (a listener with SYN/ACK, `accept` and a reset for a closed port) and a DNS cache, but no DHCP, no IPv6, no reassembly of fragments and no congestion window; the retransmission is a fixed 400 ms and the driver polls. | `ports/port_x86_64/src/net.c`, `ports/port_x86_64/src/virtio_net.c`, `components/net/src/dns.c` | partial | DHCP and IPv6 when a network needs them, a congestion window, the reassembly of fragments, and the interrupt of the PCI line when K8 routes it. |
| No store: `k_port_kv_get`/`set`/`next` answer "no" (`commit` answers 0). | `ports/port_x86_64/src/port_x86_64.c` | open | A place to keep a few keys (the RAM volume or a medium), and the same tests as the host store. |
| One `errno` for all tasks in the kernel of the board (the x86-64 port now copies the TLS block per task and follows it with `IA32_FS_BASE`). | `components/port_esp32`, `ports/port_host` | open | A thread pointer per task on the ports whose tasks share the block (the host has threads of its own). |
| The refusal paths of `k_port_exec_enter()` are not covered on the host: only a good image goes through `make x86-smoke`. | `ports/port_x86_64/src/port_exec.c`, `tests/cases/test_exec.c` | open | A host test of the checks (an image past the window, a bad entry, a bss over the ceiling) or a port test of the refusals. |
| A program is a task of the scheduler now: `execve` makes it, waits for it and the shell is asleep until it exits (K4). A program that calls `execve` starts a second task instead of replacing itself. The descriptors of the caller are inherited now (ADR 0017); the status of the program is still not handed back to the caller (0 means "it ran"). | `ports/port_x86_64/src/port_exec.c`, `components/kernel/src/ksys.c` | partial | The POSIX shape of `execve`: replacing the caller, and the status of the program as the answer of the call or of `waitpid`. |
| `exec_enter` accepts a hand-built `struct k_exec_image` and copies from it; the parser's checks are repeated there, but the structure is not tied to the bytes the caller holds. | `ports/port_x86_64/src/port_exec.c` | open | One owner of the validation (the parser) and a port that takes only a parsed image, or a documented rule that the port repeats the checks. |
| No job control: there are no process groups, no `&`, `fg`, `bg`, and `kill` takes one positive pid. Ctrl+C reaches the program the shell waits for, and every stage of a pipeline and the programs they started (the job of ADR 0017), but there is no `SIGKILL` for a program that ignores `SIGINT`, so a hung program needs `reboot`. | `components/kernel/src/task.c`, `components/kshell`, ADR 0015, ADR 0017 | open | A group of the terminal in `struct task`, a shell that starts a program in the background, `waitpid` with `WUNTRACED`/`WCONTINUED`, `SIGKILL`/`SIGSTOP` beside the four signals, and the commands `&`, `fg`, `bg`, `jobs`. |
| A pipeline has at most three stages: a boundary is a pipe whose two ends are descriptors of the shell task, and the table of a task has four. | `components/kshell/src/kshell_pipe.c`, `components/kernel/include/kernel/task.h`, ADR 0017 | open | A table of descriptors that grows with the task (or a place for the ends of a pipe that is not a descriptor), and a chain longer than two boundaries. |
| The grammar of the shell is words and no quotes: `&&`, `\|\|`, `;`, `&`, `2>&1`, globs and variables are refused or read as arguments, and the forms `a\|b` and `>file` are not operators. | `components/kshell/src/kshell_pipe.c`, `docs/wiki/shell.md` | open | A lexer with quotes and escapes, a parser of lists, and the words the shell refuses today. |
| Two commands of the shell that keep static buffers (`ls`, `rm`, `dmesg`, ...) share them when they run as stages of the same chain; two shells (the console and telnet) already did. | `components/kshell/src/kshell_files.c`, `components/kshell/src/kshell.c` | open | The buffers of a command in the task of the stage (the heap, the PSRAM or a smaller line), or a lock that does not deadlock on a full pipe. |
| A task of the kernel that never enters a system call is only checked for signals by the tick of x86-64, and even there only a program of ring 3 is checked; the shell of the host and of the board answers its marks at its own calls. | `components/kernel/src/task.c`, `components/kernel/src/sched.c`, ADR 0015 | open | A check of the running task in the tick of every port, with the handler of a kernel task run in place. |
| The four signals are a closed set: `SIGUSR1`, `SIGUSR2`, `SIGHUP` and the rest of POSIX are `EINVAL`, and a program that wants an application signal has to use `SIGINT` or `SIGTERM`. | `components/kernel/include/kernel/signal.h` | open | More slots in `sig_handlers` (the table has eight) and the names in `signal_by_name` of the shell. |
| While the shell waits for a program, a key of the console that is not Ctrl+C is dropped: there is no typeahead and no line editing across an `exec`. | `ports/port_x86_64/src/port_exec.c`, `ports/port_x86_64/src/port_x86_64.c` | open | A small buffer of the console that a program or the next prompt reads, and a foreground program that reads it. |

## K6 - The filesystem and the network

| Debt | Where | Status | What it needs |
|---|---|---|---|
| Timestamps of a file: the record of MicroFS version 2 keeps `mtime` and `ctime` and `statx` (26) reports them; a version 1 volume mounts and reports zero. | `components/mfs/`, `components/kernel/src/clock.c`, ADR 0013 | done | Nothing; an access time would need a write on a read. |
| FAT is writable but has no journal: a power cut in the middle of a write can leave a cluster no chain names or a half-written entry. `fsck fat -y` repairs the safe part of that (lost clusters in both copies, loops, cross-links, a cluster outside the volume, a broken long name, a size against its chain) and refuses to guess the rest (the boot sector, two table copies that differ, a directory whose head is shared). It has no mode, no owner, no time and one volume at a time. | `components/fs_fat32/`, `components/kernel/include/kernel/vfs.h` | partial | A journal or a cache of the changes, the moments of `statx` for a FAT entry, and `rename` when the VFS has it. ADR 0009. |
| ext2 is read-only and one volume at a time: `write`, `mkdir`, `unlink`, `chmod` and `chown` are `KERR_NOTSUP`; a journal is not replayed (ext3 and ext4 are refused), a symbolic link is not followed, and there is no `fsck ext2`. A listing cuts a name at the 32 bytes of `struct vfs_dirent`, though the format holds 255. | `components/fs_ext2/`, `components/kernel/include/kernel/vfs.h` | partial | A writable ext2 with a journal and a check, a link the VFS can follow, and a wider name of a listing. ADR 0019. |
| The board builds the ext2 reader with `CONFIG_MICRO_OS_FS_EXT2=y` but its boot (`main/src/main.c`) registers only MicroFS and FAT, so the shell cannot `mount ... ext2` on the board until the boot calls `ext2_vfs_register_type()`. The x86-64 port registers it when its option is on. | `main/src/main.c`, `components/fs_ext2/` | open | One registration call behind the flag, next to the FAT one. ADR 0019. |
| The check of a FAT volume claims the clusters in a static map of 65536 bits: a volume with more clusters is refused instead of being walked with a map that would hide a cross-link. | `components/fs_fat32/src/fat_check.c` | open | A map that grows with the volume (the PSRAM) or a walk that finds a cross-link without one. |
| `mount`/`umount` and `/etc/fstab` exist; the options of a line are not acted on (only `defaults` is accepted), a mount at the console is not written back to the table, and MicroFS is still one volume at a time (the type `mfs` reads the medium of the singleton). | `components/kernel/src/vfs.c`, `components/kshell/src/kshell_files.c`, `components/mfs/src/mfs_vfs_mount.c` | partial | Options that the filesystems can honour (`ro` first), a write of the table, and more than one volume of MicroFS. |
| MicroFS has no journal: a power cut in the middle of a write is still a volume the check has to repair. `fsck -y` puts right what the format itself proves (an orphan block, an entry or a name nobody can reach, a loop or a chain outside the data section, a shared block - the first owner keeps it - a size against its chain, the counter of a directory, a record no name reaches) and refuses to guess the rest: a damaged header, the data of a half-written file (a record and a file have no checksum) and a directory whose head another object holds stay `(not repaired)`. The repair maps 32768 records, so a volume with a bigger table is reported instead of repaired. | `components/mfs/src/mfs_check.c`, `components/kshell/src/kshell_files.c` | partial | A journal, a checksum per record or per block, and a map that grows with the volume (the PSRAM) for a table beyond 32768 records. ADR 0009 and ADR 0013. |
| The record capacity of a volume is a constant of the mount (256, `MFS_MOUNT_RECORDS`); the formatter cannot size the table from the free space, and a volume cannot ask for more records later. | `components/mfs/src/mfs_vfs_mount.c`, `components/mfs/src/mfs.c` | open | Compute the count from the volume at format time, and a documented way to grow the table (or a refusal that says so). |
| The HTTP page is not started by the boot and the `400` path has no host test: the command `httpd [port|off]` is manual, and a close with an unread tail can reset the loopback. The page runs on the board and on x86-64 (the smoke test of the port serves it through `hostfwd`), and the refusals of a well-formed request are tested; the `400` of a request that never ends is not. | `components/net/src/net_http_server.c` | open | A decision of the platform (init script or policy), a drain before the close, and a test of the request that does not end. |
| No TLS, no IPv6 and no raw sockets. | `kernel/port/net.h`, `docs/architecture.md` section 7 | decision | None for the board on the network of its owner; revisit when a public network is a requirement. |

## K7 - Quality

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No fuzz of the syscall ABI and of the parsers of the filesystem: MOX1 and the edge of `exec` have a deterministic seed (`tests/cases/test_fuzz.c`), the records of MicroFS have none. | `tests/`, `components/kernel/src/ksys.c`, `components/mfs` | open | A deterministic harness for the records of the filesystem, seeded from a formatted volume and the corpus. |
| `g_last_error` in the VFS is a write-only global written by every call of every task. The host stress of the campaign (`make soak-host`) runs several tasks over the VFS at once and TSan reports the race 91 times in a minute; nothing reads the variable, so no answer is wrong today, but the write is undefined behaviour under the C memory model and a future reader would read another task's error. | `components/kernel/src/vfs.c` | open | Remove the variable (the callers already get the code as the answer), or keep the error in the task and read it through the thread pointer. The host session of the campaign stays a blocker until one of the two is done. |
| The network half of `make soak` skips NTP unless the process can bind UDP 123: the client of the kernel asks `10.0.2.2:123`, which is the loopback of the host, and an ordinary user has no privilege for the port. | `tools/soak.py`, `components/net/src/net.c` | open | A runner with `CAP_NET_BIND_SERVICE` (or a network namespace) for the campaign, or a port option in the client of the test. The report says `skip`, so a session without it is not a silent pass. |
| The FAT half of `make disk-stress` needs `mkfs.vfat` (dosfstools) and `mcopy` (mtools) on the machine that runs the campaign; without them the half is skipped with a line, and `--require-fat` turns it into a failure for a runner that must have them. | `tools/disk_stress.py`, `Makefile` (`disk-stress`) | decision | None for the tool: a file image of FAT is not written by this tree (the tests build their own bytes), and a second formatter in Python would be a second definition of the format. The runner of the release installs the two packages. |
| The coverage gate is line coverage of `components/kernel` only: branches and functions are not counted, and the components outside the kernel (kshell, mfs, net, the ports) are not in the threshold. `panic.c` and `power.c` are at 0% because the tests do not take the machine down or sleep it, and `kmod.c` is at 55.6%; the last is the next place to test. | `tools/coverage_report.py`, `Makefile`, `components/kernel/src/panic.c`, `src/power.c`, `src/kmod.c` | open | A branch view next to the lines, the components in a report of their own, and a test of the module loader that reaches its refusals. |
| TSan reported a race between the write of `task_signal_notify_parent` and the read of `task_signal_check` on the flag of a task in the pipe tests. Three runs after it passed without a report, so the window is small; the kernel is out of this pass. | `components/kernel/src/task.c:718`, `tests/cases/test_kshell_pipe.c` | open | A lock (or an atomic) around the flag of the signal, and a loop of the pipe case under TSan that makes the window certain. |
| The reproducible gate builds `programs.img` twice on one machine. The firmware is checked in the cloud (a second `idf.py -B build-repro build` and a `cmp` of the three images, and two bundles with a `diff`), but no local target does it: that needs ESP-IDF and a second clean build. | `Makefile` (`reproducible`), `tools/bundle.sh`, `.github/workflows/ci.yml` | open | A `make reproducible-firmware` that finds ESP-IDF the way `make idf-build` does, builds twice and compares the three `.bin`. |
| `tools/device-test.sh` is outside CI: no runner has the board. | `.github/workflows/ci.yml`, `tools/device-test.sh` | open | A self-hosted runner with the board, or a documented manual gate for a release. |
| The budget of the image is checked; the budget of the SRAM is not. | `tools/integrity_check.py` (`check_budget`), `docs/roadmap.md` K7 | open | A static or a boot-time check of the internal RAM (the stacks, the drivers, the kernel) next to the image budget. |
| The null probe cannot catch a libc call: it is linked with the libc of the host, so `printf` in portable code would link. | `ports/port_null/README.md`, `tools/integrity_check.py` | open | Scan the portable sources for the names of the libc, or link the probe without the libc. |
| The host port cannot reproduce preemption: the critical section is one recursive `pthread` mutex, so a race is invisible unless TSan looks for it. | `ports/port_host/README.md` | open | A model of preemption, or TSan as the only detector (it is in the cloud CI). |

## K10 - The settings of a machine

| Debt | Where | Status | What it needs |
|---|---|---|---|
| A system file is at most 127 characters, because it is one value of the store of the port. Four keys and their comment fit; a bigger file is refused and the defaults are used. | `components/settings/include/settings/settings.h`, `kernel/port/kv.h` | decision | A larger value of the store (a change of the contract of the port) together with a file that needs it. |
| `config set` rewrites a file with one write on a truncating open: a power cut in the middle leaves a short file. The VFS has no `rename`, so a temporary file and a copy have two windows instead of one. | `components/settings/src/settings.c`, ADR 0021 ("Writing a file") | decision | Nothing for a file of a few lines: the next boot reads the lines that are there and `config sync` restores the text of the store. A checksum if a torn file ever matters. |
| `display.brightness` is kept on the board and not applied: the backlight is one pin, on or off. | `components/drivers/src/drv_display.c` | open | A PWM channel on `PIN_LCD_BL` (the driver already depends on `esp_driver_ledc`), or a panel with a dimmer. |
| The host has no applier and a run with a screen file still has none: the load names the three systems and keeps the files. | `ports/port_host/src/sim_main.c`, `components/graphics` | decision | None until the screen file of the simulator is a screen a person configures; the serial console has no attributes. |
| The store drops the last line ending of a value, so a system file that comes back after a format loses its final `\n`. | `components/config/src/config.c` (`read_value`) | decision | Nothing: a value is one line of text, the parser reads the file with or without the line ending, and no key is lost (the test compares with it off). |

## The signature of an image

| Debt | Where | Status | What it needs |
|---|---|---|---|
| The board's check is electronic, not a boot-chain check: the ROM and the ESP-IDF bootloader verify the checksum of the application and enter it, and the app verifies the trailer in `app_main` before MicroFS and the shell. The first instructions of a damaged image have already run when the sentence appears, and secure boot v2 (eFuses) or a custom bootloader with a larger partition would be needed for the chain itself. | `components/port_esp32/src/image_check.c`, `main/src/main.c`, ADR 0020 section 6 | open | Secure boot v2 with a documented key ceremony (eFuses, irreversible), or a custom bootloader component and a partition layout that fits Ed25519 and SHA-512 in 28 KB. |
| The release workflow reads `MICROOS_SIGN_KEY` from the secret of the repository and writes the material to a file before the build; a build without it uses a development key and says so. The secret is set; the owner and the rotation of the key are a duty of the release, not a line of code. | `.github/workflows/release.yml`, `tools/bundle.sh` | done | None for the mechanism; a documented rotation is the next key ceremony. |
| Only the x86-64 kernel and the board application are signed (the merged image is signed for the person or the tool). A `.mox`, a MicroFS volume, a FAT volume and `/etc` have no signature: a program is a file of a volume, and `execve` trusts the mode bit and the parser. | `components/kernel/src/exec.c`, `tools/mkfs_mfs.c`, ADR 0020 section 3 | open | A field in the program format or a detached signature beside the file, a key in the kernel, and a policy for the files a user writes. |
| Anti-rollback exists only on the board and only as a setting: `MICRO_OS_ANTIROLLBACK` keeps a floor in the NVS, which a person with a flash writer can erase. x86-64 has no store that survives a reset, so a rollback there is allowed and there is no flag. | `components/port_esp32/src/image_check.c`, `ports/port_x86_64/loader/verify.c`, ADR 0020 section 7 | open | An eFuse counter (secure boot v2) for a floor a flash writer cannot erase, and a store on x86-64 before the flag can mean anything there. |
| The key of a development build is generated per build directory: two builds of the same sources with no `MICROOS_SIGN_KEY` produce different images and different loaders. | `ports/port_x86_64/CMakeLists.txt` | decision | None: a reproducible build names a fixed key, and a key that is not a secret must not be committed. `make x86-build MICROOS_SIGN_KEY=...` is the release path. |

## The board (ESP32, P3-P5)

| Debt | Where | Status | What it needs |
|---|---|---|---|
| The IR receiver is written (RMT on G26) but the board has no receiver, so reception is a path that no test of this tree has ever run: `ir rx` waits for an element a person wires to G26, and without one it can only prove the idle line. The transmission is checked by the frame of `ir frame` and by the eye on the emitter. | `components/drivers/src/drv_ir.c`, `docs/ports/esp32/hardware.md` | open | An element on G26 and a remote, or a receiver of the same protocol on a second board; then the loop of a frame from one board to the other is a test. |
| The sound detector is a threshold and a run of windows, not a classifier: a clap, a door and a spoken word are all an event, and the detections are not logged beyond the line of the event. | `components/drivers/src/drv_sound.c`, `sound_detect.c` | open | A level of confidence (the shape of the window, a band of frequencies) and a place for the events (the log is the place today). |
| No enclosure modes of the wearable: the panel has its rotation and the console, nothing knows how the case holds the board. | `docs/ports/esp32/roadmap.md` P3 | open | The modes of the case (the orientation, what the buttons mean in each) and the drawing of them. |
| The update is over USB: `tools/ota_usb.py` writes the inactive slot, otadata selects it and the bootloader rolls back a boot that never confirms itself. There is no update over the network, because the kernel's HTTP client is a text client and not a stream for `esp_ota_write()`. | `tools/ota_usb.py`, `components/port_esp32/src/ota.c`, ADR 0022 | partial | A streaming HTTP client that writes into the inactive slot, the same signature and rollback, and a policy for the URL. |
| The layout of 1.1 (`ota_0`/`ota_1` of 1.125 MiB, `progs` of 132 KiB, MicroFS of 5.56 MiB) is a one-time move for a board of 1.0: the old volume at `0x210000` is inside `ota_1` and cannot be read at `0x271000`. | `partitions.csv`, ADR 0022 §1 | decision | None: `flash.sh --erase-volume` (or `mkfs -y`) is the documented step, and the release notes say to back the volume up first. |
| No PMIC: power and sleep belong to the ESP32, and `HOLD` (G4) must be high after a wake. | `docs/ports/esp32/README.md` "Limits" | decision | None: the board has no fuel gauge, and the power switch is the ESP32. |
| The battery percent is a curve of the voltage, not a measurement of the charge. | `components/drivers/src/battery_soc.c` | open | A discharge model and its calibration at the known points of the cell. |
| Internal SRAM is ~100 KB for the kernel, the drivers and the stacks; there is no check of that budget (see K7). | `docs/architecture.md` section 10 | open | The SRAM half of the budget check. |

## K0 - The library

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No `mmap` and no `brk`: the memory of the tasks is shared and a program cannot ask for a range of its own. | `docs/libc.md` "What the library cannot do" | open | The address space of K5 and a call over it. |
| No `dup2`: a program cannot send its output to a file. | `docs/libc.md`, `components/libc/src` | done | `dup2` (28) and `pipe` (27) are in the ABI and the library, and an open file is a counted description (ADR 0014). Closed in 0.7. |
| A wait of a pipe is a poll with a bound of 500 ms, not a queue of waiters: a read or a write that finds nothing in time is `EAGAIN`. | `components/kernel/src/kfile.c` | open | A queue of waiters per pipe and a wakeup from the reader or the writer. |
| The descriptor 0 is not the console; a program that wants the input opens `/dev/tty0`. | `docs/libc.md`, `components/devfs/src/devfs.c` | open | Bind 0 to `tty0` for a program (or a decision that it never is). |
| `printf` of a float agrees with the host only to the precision asked for; the last digit can differ. | `docs/libc.md` | open | A `dtoa` with the exact rounding, or a documented precision. |

## K8 - Many cores (ADR 0012)

| Debt | Where | Status | What it needs |
|---|---|---|---|
| The AP bring-up of x86-64: the MADT is read (two processors of `-smp 2` are found, the local APIC address is remembered) and the boot prints `2 cpu(s) reported by the firmware, 1 in use`, but no AP is started. | `ports/port_x86_64/src/cpu.c`, `src/kmain.c`, ADR 0012 "The pilot of 1.x" | open | A trampoline in low memory (real mode, GDT, PAE, long mode, a stack), INIT-SIPI-SIPI through the local APIC, one `port_task`/TSS/thread pointer per core, the local APIC timer with its vector and the I/O APIC for the lines. |
| The critical section of the x86-64 port is `cli` and a per-core depth counter: it stops the local core only, so every kernel structure that trusts it (the task table, the log ring, the console sinks, the block devices) and the bytes of `k_port_write()` (VGA and COM1, written byte by byte) are unprotected against a second core that enters the kernel. The host is serialized by its `s_sink_lock` and its one global critical section, so the two-core runs do not see this one. | `ports/port_x86_64/src/{port_x86_64.c,port_x86_64_task.c}` | open | A real spinlock in `k_port_enter_critical()` (acquire with the interrupts saved, release in the reverse order), and a measurement that the coarse lock does not stop the second core for the length of a filesystem call. It is the first step of the AP bring-up, before the trampoline. |
| The port of x86-64 keeps one `g_current`, one TSS (RSP0 and IST) and one thread pointer for the machine: a second core would share the context of the first. | `ports/port_x86_64/src/port_x86_64_task.c`, `src/gdt.c` | open | `g_current[KERNEL_CPUS_MAX]`, a GDT/TSS per core (one descriptor per core), `ltr` with the selector of the core, and the same for `IA32_FS_BASE` and RSP0. |
| The host port's "cores" are a model: every task is a pthread, so two tasks of the same core really run at the same time, and a core is a run queue plus the `cpu` field of the task, not a processor the kernel schedules on. A task is never on two cores (one pthread, one field) and the tests check it, but the port does not constrain the machine. | `ports/port_host/src/port_host.c` | decision | None for the gate of the host: TSan and the two-core soak are the answer to "what would a real preemption do wrong". A port that wants N worker threads needs a context switch of its own (`ucontext` or a hand-off), and the kernel's scheduler already models it. |
| The kmm, the VFS, the console and the block layer were not changed: they take a sleeping `k_mutex` or the owner-and-depth lock of the kmm, and the scheduler lock is never held across their waits. The audit says they are correct under two cores as sleeping locks, but there is no test that runs a filesystem of two cores with an aggressive migration. | `components/kernel/src/{kmm,vfs,kfile,console,blockdev}.c`, `components/mfs` | partial | A stress of two cores that migrates a task in the middle of a filesystem operation (`make soak-host` does the load; the migration is the scheduler's), and a review of every `k_port_enter_critical()` that protects more than a few instructions. |
| The board is not part of this phase: its two cores run under FreeRTOS and the kernel scheduler of that port is asked to use one. | `components/port_esp32`, ADR 0018 | decision | The board's own work (ADR 0012, "The machines"), when the compatibility port is allowed to change. |
| The two calls of `kernel/port/cpu.h` have a weak default in the portable kernel (`src/cpu_default.c`, a GNU attribute): a port that does not implement them links as one core. | `components/kernel/src/cpu_default.c`, `kernel/port/cpu.h` | open | A port that reports more than one core implements both and raises `api_minor`; the default is the floor of the contract, and the weak symbols are the one GNU extension the portable kernel admits for it. |

## Decisions, not debts

* **The campaign of 1.0 is a release duty, not a push check.** `make soak`,
  `make soak-host`, `make fuzz-long`, `make disk-stress` and
  `tools/board_soak.sh` are not part of `make ci`: they take ten minutes of
  QEMU or a board, and they are run for a release candidate. The tools and
  their criteria are in [`docs/wiki/testing.md`](wiki/testing.md), "The
  campaign of 1.0".

* **The board never runs a program.** Its flash is not executable for a file,
  and internal SRAM is the memory of the kernel and the drivers; `execve`
  answers `ENOSYS` (ADR 0005).
* **No root bypass.** There is one user, uid 0, and it gets no exemption: a
  file of mode `0200` cannot be read by anybody, the only user included.
* **`chmod 000` is not expressible.** Zero in the mode bits means "the record
  predates the modes", and the reader falls back to `0644`/`0755`.
* **No TLS, no IPv6, no raw sockets.** The board is on the network of its
  owner; the contract of the port offers TCP, a server and one-shot UDP.
* **Telnet has no authentication.** The board is on the network of its owner,
  and `telnet off` closes the door.
* **MOX1, not ELF.** A flat image with a 32-byte header, linked for the window
  of the port; a loader of ELF is a chapter that a machine with 100 KB of
  SRAM does not need.
* **`stat` checks nothing.** It reports; the check happens at `open` and
  `execve`.
* **The host port cannot run a program.** Jumping into an image inside the
  simulator would run it with the libc of the host under it; the format is
  exercised there as data.
* **The board has no PMIC.** `HOLD` (G4) is the power switch of the design,
  not a missing chip.
* **The applications are not in this tree.** The sensor logger, the clock and
  the weather station are a repository of their own; a board with 8 MB of
  flash does not carry programs nobody asked for.

## Notes that were stale

This pass corrected the statements below; they are kept here so that a reader
of an older copy knows what changed.

* **The boot of x86-64 does go through `execve`.** `docs/phases/phase7.md`
  said the boot called the parser and the loader itself; `program_run()` in
  `ports/port_x86_64/src/kmain.c` calls `ksys_execve()`, and the phase report
  has said the opposite in two places. Both are fixed.
* **`drv_mic` was a stub.** `docs/architecture.md` listed it with the
  finished drivers while the module had no check and the doctor said `skip`.
  The driver is written now (PDM RX at 16 kHz, RMS and peak) and the notes
  say what it does; only the detector on top of it is still ahead.
* **The board has deep sleep, the RTC alarm wakes it and there is a factory
  reset.** This register kept three rows of the board open ("no RTC alarm and
  no deep sleep", "no sound detector", "no factory reset") that the tree had
  already closed in code and in `docs/ports/esp32/roadmap.md` P3. The sleep of
  a board whose alarm is armed sets the timer of the chip for the moment of
  the match, because the INT of the clock is not a wake line; the detector on
  top of `drv_mic` and the factory reset that writes the defaults of `/etc`
  back are in the tree. What is left of the three is the receiver of the IR
  and the case of the wearable, and those have rows of their own now.
* **README said there were no file owners at the edge.** Every record has a
  mode and an owner, `chmod`/`chown` reach them and `open`/`execve` check the
  triad; what is missing is the second user, not the ownership. The bullet is
  reworded.
* **The window of x86-64 is 16 MiB, not 32 MiB.** `docs/ports/x86_64/README.md`
  said "the window is 32 MiB at 32 MiB"; it starts at 32 MiB and ends at
  48 MiB.
* **TSan and the image budget are checked already.** `docs/roadmap.md` K7 kept
  them open; `.github/workflows/ci.yml` runs `make test-tsan`, and
  `tools/integrity_check.py` checks the image. The SRAM half of the budget
  stays open.
* **`make ci` is the same list as the cloud, TSan and `x86-smoke` included.**
  The K7 row said it was not; the Makefile has run TSan and, when QEMU is on
  `PATH`, the boot of the x86-64 port for a while, and it now adds the
  coverage gate and the reproducible image as well. Valgrind and the firmware
  build still belong to the cloud alone, and that part stays a debt above.
* **The layout of MicroFS is not "4% and one block of INFO".** `docs/memory.md`
  said `INFO` was 1 block and `META` was always 4%; the formatter puts `INFO`
  in the last erase sector (4 blocks), starts `META` at 4% and grows it until
  the bitmap and the table fit, and stores the three boundaries in the header.
  The example table and the list of improvements are corrected.
* **`v0.4.0-rc1` and `-rc2` exist.** `docs/ports/esp32/roadmap.md` P2 stopped
  at `v0.3.1`; both release candidates are named now, and `v0.4.0` is left as
  the next one.
* **The translations.** `docs/ru/roadmap_ru.md` and the other Russian files
  carry the banner that says they may lag; this document has no translation
  yet, and `docs/ru/README.md` says so.
