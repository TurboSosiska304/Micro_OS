# Kernel and ABI

The system calls are the only door between a program and the kernel. Their
numbers are a **contract**: they are written down, covered by a test, and never
change. A program built for one version of the kernel keeps working with the
next one. The canonical document, with the arguments and the reasoning, is
[`docs/syscall-abi.md`](../syscall-abi.md); this page is the map for a person
who wants to use the system, not a second copy of the contract.

## The table

| No. | Call | What it does |
|---|---|---|
| 0 | `exit` | end the task (or the program) with a status |
| 1 | `read` | bytes out of a descriptor |
| 2 | `write` | bytes into a descriptor |
| 3 | `open` | a descriptor for a path, with flags |
| 4 | `close` | drop a descriptor |
| 5 | `stat` | the type, the mode, the owner, the size |
| 6 | `lseek` | move the position of a descriptor |
| 7 | `unlink` | remove a file |
| 8 | `mkdir` | create a directory |
| 9 | `readdir` | the next entries of a directory |
| 10 | `getpid` | the number of the task |
| 11 | `getcwd` | the current directory |
| 12 | `ioctl` | a command of a device |
| 13 | `yield` | give the processor away |
| 14 | `sleep` | wait in milliseconds |
| 15 | `uptime` | milliseconds since the boot |
| 16 | `reboot` | restart the machine |
| 17 | `fork` | a child task |
| 18 | `getppid` | the number of the parent |
| 19 | `waitpid` | wait for a child, get its status |
| 20 | `malloc` | memory of the kernel heap |
| 21 | `free` | give it back |
| 22 | `realloc` | resize a block |
| 23 | `chmod` | the mode of an object |
| 24 | `chown` | the owner of an object |
| 25 | `execve` | make the caller a program, with its arguments and environment |
| 26 | `statx` | the numbers of `stat` and the moments of a change |
| 27 | `pipe` | two descriptors of one pipe |
| 28 | `dup2` | a second descriptor of an open file |
| 29 | `kill` | send a signal to a task |
| 30 | `signal` | install a handler, get the previous one |
| 31 | `sigreturn` | return from a handler (the port's page, not a program) |
| 32 | `getlimit` | the current memory, CPU or descriptor limit of the task |
| 33 | `setlimit` | lower one of them |
| 34 | `rename` | a name of an object inside one filesystem |
| 35 | `ftruncate` | cut an open file to a size |
| 36 | `getuid` | the real uid of the task |
| 37 | `geteuid` | the effective uid of the task |
| 38 | `getgid` | the gid of the task |
| 39 | `setuid` | change the uid: root names any, another task its real or saved uid |
| 40 | `setgid` | change the gid by the rule of one gid |

`41` is the count, not a call. A number that is not in the table is `ENOSYS`;
a new call always takes the next free number, and an old one never moves. The
shell prints the same table with `abi`:

```text
microos> abi
the system calls of the kernel:
nr    name
0     exit
...
25    execve
26    statx
27    pipe
28    dup2
29    kill
30    signal
31    sigreturn
32    getlimit
33    setlimit
34    rename
35    ftruncate
36    getuid
37    geteuid
38    getgid
39    setuid
40    setgid
41 call(s) registered
```

The convention is the one of Linux: the number and six arguments, the result in
the return value, and a negative result is an error with the number of POSIX
(`-14` is `EFAULT`, `-2` is `ENOENT`, `-38` is `ENOSYS`). The width of an
argument is the width of a pointer of the machine: 32 bits on the board, 64
bits on the host and on x86-64.

## The edge of the table

Every argument is checked where it enters, in
`components/kernel/src/ksys.c`, before a filesystem or a driver sees it:

* **a path** is not NULL, begins at `/`, and ends within 127 characters
  (`KSYS_PATH_MAX`, one less than `VFS_PATH_MAX`); a path that does not end in
  the window is `ENAMETOOLONG`;
* **a buffer** is NULL only with a zero length, and no call touches more than
  64 KiB in one exchange; a longer one is `E2BIG`, a NULL with a length is
  `EINVAL`;
* **the memory of the caller** is checked through the port: on a machine with
  paging the kernel walks the page tables of the program, and a range that is
  not there is `EFAULT`; the bytes are then copied with
  `copy_from_user()`/`copy_to_user()`, so the VFS never sees a pointer of the
  program;
* **the flags of `open`** are one of the three access modes and no bit outside
  the contract;
* **the `whence` of `lseek`** and **the pid of `waitpid`** are compared before
  a cast can cut a wide register into a valid value;
* **`stat` and `getcwd`** need the place for the answer: NULL is `EINVAL`, a
  place that is not the caller's memory is `EFAULT`;
* **`readdir`** takes whole entries: a count that is not a multiple of the size
  of an entry is `EINVAL`.

`EFAULT` is the most interesting of them, because it is what a machine with an
MMU can finally say. On x86-64 the null page, the guard page under the stack,
the part of the window that was never mapped and a range that runs past
`0x16000000` are all "not there": the call answers `-14` instead of taking a
page fault in ring 0. `/bin/badptr` is the program of that test - it hands
`write`, `stat` and `read` a pointer outside the window, a range that crosses
its end and the guard page, and every call answers 14 while the machine goes on
and writes a line with a valid pointer afterwards.

## Signals

Four signals, the numbers of POSIX: `SIGINT` (2), `SIGPIPE` (13), `SIGTERM`
(15) and `SIGCHLD` (17). `signal(sig, handler)` installs a function of the
program and answers the previous disposition; `SIG_DFL` ends a task that gets
`INT`, `TERM` or `PIPE` with the status 128 + the signal, `SIG_IGN` throws the
signal away, and `SIGCHLD` is ignored by default. `kill(pid, sig)` sends a
signal to one task with a positive pid; 0 and -1 are not groups here, and a pid
that names no task is `ESRCH` (`-3`, the number the table already used for
"file exists").

A signal is answered in the context of the task that gets it, on the way out of
a system call or on the tick of the timer of x86-64. A handler of a program runs
in ring 3: the port puts a frame below the stack pointer of the program and the
program returns through the restorer page, which calls `sigreturn` (29). A
handler of a task of the host or of the board is an ordinary function of the
kernel and runs in place.

`waitpid()` looks at a finished child first and then at the signals; a pending
signal that has to be answered makes the call answer `EINTR` and leaves the
child to the next wait. The exit of a child marks `SIGCHLD` on its parent.
`SIGSTOP`, `SIGTSTP` and `SIGCONT` are scheduler operations: a stopped task is
in no queue, and only CONT makes it ready again. Ctrl+C sends `SIGINT` to the
foreground process group and Ctrl+Z sends `SIGTSTP`; the shell keeps that group
as a numbered job and implements `&`, `jobs`, `fg` and `bg`. `kill` still takes
one positive pid from `ps`; group delivery is the tty and shell path. ADR 0015
has the signal design, ADR 0059 the job table, and the x86-64 shell smoke runs
the terminal keys through it.

## Descriptors

Every task has a table of sixteen descriptors, and the descriptor is the
number of the slot. The open file behind it is a counted description of the
kernel: two descriptors of one task (`dup2`) and the copies of the parent and
the child of a fork name one open file, with one position and one set of
flags. A
descriptor remembers the access it was opened with: reading one opened for
writing, or writing one opened for reading, is `EBADF` before a filesystem is
asked. The position lives in the description, so two tasks that opened their
own files do not move each other's cursor.

A directory is opened with `KSYS_O_DIRECTORY` (`0x0800`) and read with
`readdir`; it is read, never written. A plain `open` of a directory is
`EISDIR`, and `readdir` of a file is `ENOTDIR`.

`pipe(fds)` (27) gives two ordinary descriptors, `fds[0]` reading and `fds[1]`
writing; the pipe is a ring of the port, 64 KiB on x86-64 and 4 KiB on the
board, taken from the allocator with the pipe (ADR 0035). The end of the
writing end is the end of the file (a read gives 0), and a write with no
reader is `EPIPE` - there is no `SIGPIPE` yet, so the error is the whole
answer. A read of an empty pipe sleeps until the bytes arrive, a write of at
most `PIPE_BUF` bytes lands whole, and a signal cuts a wait short with
`EINTR`; `lseek` of a pipe is `ESPIPE` and `readdir` is `ENOTDIR`.
`dup2(oldfd, newfd)` (28) makes `newfd` a second descriptor of one open file,
closes what was open there, and answers `newfd`; a number outside the table
is `EBADF`. [ADR 0014](../adr/0014-pipes.md) has the design and
[ADR 0035](../adr/0035-the-grain-of-the-io.md) the ring and the wait of 1.6.

## Limits

A task starts with the ceiling of three resources and may only lower them:
the memory it holds (2 MiB on the board and the host, the window of the
programs on x86-64), the processor time it spends (about 49 days) and
the descriptors it opens (16). `setlimit` (33) takes a selector
(`KSYS_RLIMIT_MEMORY`, `_CPU`, `_FDS`) and a value; `getlimit` (32) copies the
current limit to the caller. A raise is `EPERM`: the ceiling belongs to the
machine, and even root cannot lift a limit.

The memory is the sum of the user blocks of the task - the kernel and the
system utilities are not charged - and a block over it is 0 from `malloc`
(`ENOMEM` from the library); `free` returns the room. The CPU time is counted
at the system calls and at the tick of the port; when it reaches the limit the
task is marked with `SIGTERM` (the closest of the four signals to `SIGXCPU`)
and the default action ends it with 143. The descriptors stop at the limit
with `EMFILE`: `open`, `pipe` and a `dup2` onto a number at or above it.
Lowering a limit never closes what is already open.

On the machine with page tables the memory limit is the ceiling of the whole
address space of a program: `brk` is charged to it as it grows, an anonymous
mapping is counted when it is made and given back when it is unmapped, and
the stack of a program stops at it too - a program that lowers its limit to
128 KiB has a heap, a mapping and a stack that each stop there. A page that
the program asks for as writable and executable at once is `EINVAL`, not a
page: a page is code or it is data (ADR 0052).

The library has `microos_getlimit`, `microos_setlimit` and
`microos_getrlimits`, the shell has `ulimit` to print and lower the limits of
the shell, and [`docs/syscall-abi.md`](../syscall-abi.md) has the contract.

## The mode and the owner

Every object has nine bits of `rwx` and a uid of fifteen bits, stored in the
spare bits of its MicroFS record ([`docs/memory.md`](../memory.md#6-the-record-of-an-inode)).
A new file is `0644`, a new directory `0755`, and the owner is 0.

Every task has a real, an effective and a saved **uid** and a **gid**,
inherited from the task that made it (`getuid` 36 is the real uid, `geteuid`
37 the effective one that every check reads, `getgid` 38, `setuid` 39,
`setgid` 40). Root is uid 0 and it passes the triads of read and write: a file
of mode `0200` is read by root, and one of `0400` is written by it. An object
owned by the effective uid of the task is judged by the triad of the owner,
one whose group is the gid of the task by the triad of the group, and every
other by the triad of the others. The execute bit is the exception: `execve`
needs an `x` bit for somebody, root included. An image with the setuid flag of
`MOX1` runs with the identity of the owner of its file for the length of the
run (ADR 0053). `open` compares its access with the mode before a descriptor
exists, so every filesystem answers a forbidden open the same way (`EPERM`);
`stat` reports and checks nothing, as in POSIX. `chmod` and `chgrp` are the owner or root,
`chown`, `mount` and `umount` are root, and a signal to a task of another uid
is `EPERM`.

A mode outside the nine bits and an owner outside the fifteen are `EINVAL`, not
a number cut to fit. `chmod 000` is not expressible: a zero in the record means
"this record predates the modes", so the filesystem refuses it instead of
writing a number it would read back as `0644`.

The `x` bit of every directory of a path is walked and checked before the
object at the end: a directory of mode `0600` keeps its children out of reach
of `open`, `stat` and `execve`.

## The moments of a file

`stat` has no field for a date and its structure is frozen. `statx` (26) takes
a `struct vfs_statx`: the five fields of `stat` and then `mtime` (the last
change of the content) and `ctime` (the last change of the record, the mode or
the owner), both the seconds of Unix in 64 bits. They come from the clock of
the kernel; a machine that was never set stamps with the seconds since its
boot, and `k_clock_present()` says whether the number can be printed as a
date.

Zero means **this filesystem keeps no time**: a volume of MicroFS of version 1
has 16-byte records with no room for a moment, and procfs, devfs and the FAT
reader have no moment of their own. A new MicroFS volume (version 2) moves
`mtime` on create, write and truncate, and `ctime` on those and on
`chmod`/`chown`; there is no access time, because a read does not rewrite a
record. The library has the same call as `microos_statx()` and `struct
microos_statx`. ADR 0013 has the format and the migration: an old volume
mounts and stays timeless, it is not converted behind its owner's back.

## Processes and programs

`fork()` answers the pid in the parent and 0 in the child. On x86-64 it is a
real copy-on-write fork of the page tables; on a machine without an MMU the
child shares the memory of the parent and re-enters its program, and the port
says so. `waitpid()` waits for a child and reports its status; no child is
`ECHILD`.

`execve(path, argv, envp)` reads the file through the VFS, checks the mode,
parses a `MOX1` image and asks the port to run it. The environment is the third
argument, a vector of `NAME=VALUE` words; a NULL one is the environment of the
system (`PATH=/bin`, `HOME=/`, `USER=root`, `PWD=/`). The port enters the
program with `argc` in `rdi`, `argv` in `rsi` and `envp` in `rdx`
([ADR 0016](../adr/0016-userland.md)). The board and the simulator answer
`ENOSYS` for an image of their own machine and `ENOEXEC` for one of another;
x86-64 enters ring 3. [programs.md](programs.md) has the details of the
format, the entry and the library of a program.

## The contract of the port: 1.8

The kernel and a port are two halves of one pair, and the version of the
contract is their vermagic. It lives in
[`info.h`](../../components/kernel/include/kernel/port/info.h) of the port
contract:

```c
#define KERNEL_PORT_API_VERSION_MAJOR 1u
#define KERNEL_PORT_API_VERSION_MINOR 8u
```

A port asserts the version at build time, and the boot compares it again and
panics on a major mismatch instead of failing far from the cause. `make
integrity` checks that every port implements the whole contract. The step
1.7 was the check of the memory of the caller - `k_port_exec_contains()`,
which turns a bad pointer of a program into `EFAULT`; 1.8 is
`k_port_exec_arch()` - the machine of the port as the header of an image names
it (`0` x86-64, `1` Xtensa, `-1` for a port that runs no programs), which is
what turns an image of another machine into `ENOEXEC` before a byte of it runs
(ADR 0018). Some older pages of `docs/` still name 1.4; the number the boot
prints is the one in `info.h`, and the boot of this tree prints `contract 1.9`.

The three builds of one tree are the check of the contract in practice: a
function that only one port implements is a function the others do not need,
and a component that includes a platform header fails `make integrity` before
it can compile on the machine the author has.

[Back to the wiki index](README.md)
