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

`32` is the count, not a call. A number that is not in the table is `ENOSYS`;
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
32 call(s) registered
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
`0x03000000` are all "not there": the call answers `-14` instead of taking a
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
child to the next wait. The exit of a child marks `SIGCHLD` on its parent. No
job control: the system has no groups and no `&`, `fg` or `bg`; Ctrl+C while
the shell runs a program sends `SIGINT` to that program, and `kill` takes a pid
from `ps`. ADR 0015 has the design, and the x86-64 smoke test runs `/bin/signals`
through all of it.

## Descriptors

Every task has a table of four descriptors, and the descriptor is the number of
the slot. The open file behind it is a counted description of the kernel: two
descriptors of one task (`dup2`) and the copies of the parent and the child of
a fork name one open file, with one position and one set of flags. A
descriptor remembers the access it was opened with: reading one opened for
writing, or writing one opened for reading, is `EBADF` before a filesystem is
asked. The position lives in the description, so two tasks that opened their
own files do not move each other's cursor.

A directory is opened with `KSYS_O_DIRECTORY` (`0x0800`) and read with
`readdir`; it is read, never written. A plain `open` of a directory is
`EISDIR`, and `readdir` of a file is `ENOTDIR`.

`pipe(fds)` (27) gives two ordinary descriptors, `fds[0]` reading and `fds[1]`
writing; the pipe is a ring of 256 bytes in the kernel. The end of the writing
end is the end of the file (a read gives 0), and a write with no reader is
`EPIPE` - there is no `SIGPIPE` yet, so the error is the whole answer. A wait
of a pipe is a poll with a bound of half a second and then `EAGAIN`; `lseek`
of a pipe is `ESPIPE` and `readdir` is `ENOTDIR`. `dup2(oldfd, newfd)` (28)
makes `newfd` a second descriptor of one open file, closes what was open
there, and answers `newfd`; a number outside the table is `EBADF`.
[ADR 0014](../adr/0014-pipes.md) has the design and the limits.

## The mode and the owner

Every object has nine bits of `rwx` and a uid of fifteen bits, stored in the
spare bits of its MicroFS record ([`docs/memory.md`](../memory.md#6-the-record-of-an-inode)).
A new file is `0644`, a new directory `0755`, and the owner is 0.

The system has **one user**, uid 0, and it gets **no exemption of root**: a file
of mode `0200` cannot be read by the only user of the system either. An object
owned by uid 0 is judged by the triad of the owner, one owned by any other
number by the triad of the others, because there is no group here. `open`
compares its access with the mode before a descriptor exists, so every
filesystem answers a forbidden open the same way (`EPERM`); `execve` needs the
`x` bit; `stat` reports and checks nothing, as in POSIX.

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
`ENOSYS`; x86-64 enters ring 3. [programs.md](programs.md) has the details of
the format, the entry and the library of a program.

## The contract of the port: 1.7

The kernel and a port are two halves of one pair, and the version of the
contract is their vermagic. It lives in
[`info.h`](../../components/kernel/include/kernel/port/info.h) of the port
contract:

```c
#define KERNEL_PORT_API_VERSION_MAJOR 1u
#define KERNEL_PORT_API_VERSION_MINOR 6u
```

A port asserts the version at build time, and the boot compares it again and
panics on a major mismatch instead of failing far from the cause. `make
integrity` checks that every port implements the whole contract. The latest
step, 1.7, is the check of the memory of the caller -
`k_port_exec_contains()`, which is what turns a bad pointer of a program into
`EFAULT`. Some older pages of `docs/` still name 1.4; the number the boot
prints is the one in `info.h`, and the boot of this tree prints `contract 1.7`.

The three builds of one tree are the check of the contract in practice: a
function that only one port implements is a function the others do not need,
and a component that includes a platform header fails `make integrity` before
it can compile on the machine the author has.

[Back to the wiki index](README.md)
