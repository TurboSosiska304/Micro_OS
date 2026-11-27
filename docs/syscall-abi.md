# The ABI of the system calls

> Russian version: [ru/syscall-abi_ru.md](ru/syscall-abi_ru.md) (it may lag).

The system calls are the interface between the kernel and the programs. Their
numbers are a **contract**: they are written down here, they are covered by a
test, and they never change. A program that was compiled for one version of
the kernel keeps working with the next one.

## The convention

The same as in Linux: the number of the call and six arguments, the result in
the return value.

```c
k_ssize_t ksys_call(k_u32_t number, k_u64_t a0, k_u64_t a1, k_u64_t a2,
                     k_u64_t a3, k_u64_t a4, k_u64_t a5);
```

The width of the arguments is the width of a pointer of the target:
**32 bits on the board** (the kernel of the ESP32 works with 32 bit numbers,
it is faster) and 64 bits on a 64 bit host. The numbers of the calls are
the same everywhere, only the width of the arguments belongs to the target,
like in the real syscall interface.

A pointer must never be cut in half: that was a bug that only showed up on
the host, where the pointers are twice as long.

A negative result is an error, the numbers are the negative error codes of
`kernel/errno.h` (the same values as the POSIX ones).

## The entry into the kernel

On the board and on the simulator a program is a function of the kernel and
calls it directly. The x86-64 machine runs a program that the kernel did not
compile, so the call has to be an instruction of the processor. The port of
that machine offers two doors to the same convention (ADR 0006):

| What | Where |
|---|---|
| the instruction | `syscall` (the fast door), `int $0x80` (the gate) |
| the number of the call | `rax` |
| the six arguments | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| the result | `rax` (negative is an error) |

The library of the tree takes the fast door: the instruction `syscall` jumps
to the address the port keeps in `IA32_LSTAR`, with the flags of `IA32_FMASK`
cleared (the interrupt flag first of all), the return address of the program
in `rcx` and its flags in `r11`. The instruction changes no stack, so the
entry takes the kernel stack of the program from the record of the core that
`IA32_GS_BASE` names - the same number the port writes into `RSP0` of the TSS,
so both doors land on the same stack. The entry pushes the five words of the
crossing and the fifteen registers the caller may lose across a C call, which
is the frame of 160 bytes the gate leaves too, puts the kernel's thread
pointer back, turns the interrupts on (they are off for the first
instructions, and on again for the call itself, which is what lets `sleep` and
`yield` really wait) and calls `ksys_call()`.

The return is `sysret`, which takes the address from `rcx` and the flags from
`r11`, and it is guarded: an address that is not canonical, or a frame whose
selector is not a program's, takes the `iretq` of the gate instead, because
the frame a `sigreturn` restores comes from memory the program can write and
an address the instruction cannot return to faults in ring 0 after the stack
of the program is in `rsp`.

The gate of the vector 128 of the table of the interrupts stays, and it is
not a leftover: it is what a program that was not built against this library
uses, and it is the door the restorer of a signal handler returns through. The
gate opens for ring 3, because the x86-64 port runs a program as a user with
page tables of its own (ADR 0007); the processor takes the `RSP0` of the TSS
on the way in, so the handler runs on a kernel stack of the program and not on
the stack of the program, which ring 3 could have pointed anywhere. It builds
the same frame, runs the same body and returns through the same tail, which
picks `sysret` for a program's frame and `iretq` for anything else, setting
the thread pointer to zero first because the kernel's TLS block is not mapped
in ring 3.

The registers are the ones of Linux: `rcx` and `r11` are not arguments
because the instruction `syscall` of a 64-bit machine keeps them for the
return address and the flags, and one convention for both instructions costs
nothing. They are clobbered by a call, as they are on Linux: the frame of the
call keeps them, so a signal that arrives inside one shows the address and the
flags it interrupted, and no program of the system reads them back.

A program that was entered with an image starts with the arguments of a C
function in the registers (ADR 0016): the count of the arguments in `rdi`, the
vector of the arguments in `rsi` and the vector of the environment in `rdx`.
The stack is aligned as after a call and its top word is zero, so the entry of
a program is `int main(int argc, char *const argv[], char *const envp[])`; a
program that returns instead of calling `exit` faults on address zero.

A program ends with `exit` (number 0): the kernel asks the port whether a
program is in flight (`k_port_exec_active()`), and the port unwinds to the
call that entered it with the status. `k_exec_exit()` carries the number,
`k_exec_run()` answers with it, and `execve` answers 0 - the machine has no
process to exit into yet, so the call ends and the caller sees the program
finish. On the board and in the simulator, where a program is a task, `exit`
still ends the task. A program that returns with `ret` instead of calling
`exit` pops the zero at the top of its stack and faults; the port reports the
fault and recovers, but the status is 255 and not the number the program
chose.

## The table

| Number | Call | Arguments | Result |
|---|---|---|---|
| 0 | `exit` | `code` | never returns |
| 1 | `read` | `fd, buf, count` | the number of the bytes, or an error |
| 2 | `write` | `fd, buf, count` | the number of the bytes, or an error |
| 3 | `open` | `path, flags` | the descriptor, or an error |
| 4 | `close` | `fd` | 0, or an error |
| 5 | `stat` | `path, buf` | 0, or an error |
| 6 | `lseek` | `fd, offset, whence` | the new position, or an error |
| 7 | `unlink` | `path` | 0, or an error |
| 8 | `mkdir` | `path` | 0, or an error |
| 9 | `readdir` | `fd, buf, count` | the number of the bytes, or an error |
| 10 | `getpid` | - | the number of the task |
| 11 | `getcwd` | `buf, size` | the length of the path, or an error |
| 12 | `ioctl` | `fd, cmd, arg` | 0, or an error |
| 13 | `yield` | - | 0 |
| 14 | `sleep` | `ms` | 0 |
| 15 | `uptime` | - | the time since the boot in milliseconds |
| 16 | `reboot` | - | never returns |
| 17 | `fork` | - | the number of the child, or an error |
| 18 | `getppid` | - | the number of the parent, 0 at the root |
| 19 | `waitpid` | `pid, status` | the number of the child, or an error |
| 20 | `malloc` | `size` | the address of the memory, 0 when there is none |
| 21 | `free` | `ptr` | 0 |
| 22 | `realloc` | `ptr, size` | the address of the memory, 0 when there is none |
| 23 | `chmod` | `path, mode` | 0, or an error |
| 24 | `chown` | `path, owner` | 0, or an error |
| 25 | `execve` | `path, argv, envp` | the caller becomes the program and never returns; 0 when a program started for a kernel task returns; or an error |
| 26 | `statx` | `path, buf` | 0, or an error |
| 27 | `pipe` | `fds` | 0 and two descriptors, or an error |
| 28 | `dup2` | `oldfd, newfd` | `newfd`, or an error |
| 29 | `kill` | `pid, sig` | 0, or an error |
| 30 | `signal` | `sig, handler` | the previous disposition, or an error |
| 31 | `sigreturn` | - | the saved value of the interrupted call |
| 32 | `getlimit` | `which, value` | 0 and the limit in the place of the caller, or an error |
| 33 | `setlimit` | `which, value` | 0, or an error |
| 34 | `rename` | `from, to` | 0, or an error |
| 35 | `ftruncate` | `fd, size` | 0, or an error |
| 36 | `getuid` | - | the real uid of the task |
| 37 | `geteuid` | - | the effective uid of the task |
| 38 | `getgid` | - | the gid of the task |
| 39 | `setuid` | `uid` | 0, or an error |
| 40 | `setgid` | `gid` | 0, or an error |
| 41 | `rmdir` | `path` | 0, or an error |
| 42 | `fstat` | `fd, buf` | 0, or an error |
| 43 | `poll` | `fds, nfds, timeout` | the count of the ready entries, 0 on a timeout, or an error |
| 44 | `mmap` | `addr, len, prot, flags, fd, offset` | the address of the range, or an error |
| 45 | `munmap` | `addr, len` | 0, or an error |
| 46 | `msync` | `addr, len` | 0, or an error |
| 47 | `brk` | `addr` | the new end of the heap, or an error |
| 48 | `chdir` | `path` | 0, or an error |
| 49 | `clock_gettime` | `clock, buf` | 0 and the moment in the place of the caller, or an error |
| 50 | `getrusage` | `buf` | 0 and the use of the processor in the place of the caller, or an error |
| 51 | `chgrp` | `path, group` | 0, or an error |
| 52 | `getpgid` | `pid` | the process group of `pid` (0 for the caller), or an error |
| 53 | `setpgid` | `pid, pgid` | 0, or an error |
| 54 | `fsync` | `fd` | 0, or an error |

The numbers 0 to 22 are frozen, 23 to 26 were the next free ones; pipes took
27 and 28 (ADR 0014), signals the three after them (ADR 0015), the limits of
a task 32 and 33, the atomic rename 34 (ADR 0024 §5), the cut of an open
file 35 and the credentials of a task 36 to 40 (the second half of 1.4),
`rmdir` and `fstat` 41 and 42 (the filesystem of 1.5), the wait for a
descriptor 43 and the memory a program maps 44 to 47 (the first lane of 1.6).
The lane of 1.8 added the directory of a task 48, the clock 49 and the use of
the processor 50; the three are below. The group of an object took 51 beside
the owner (ADR 0044). The process group of a task took 52 and 53, the minimal
slice of job control: a tty has a foreground group and Ctrl+C is delivered to
it. `fsync` took 54: it brings the data and the metadata of an open file to
the medium and answers `KERR_NOTSUP` when the medium or the filesystem cannot
make that promise. A call that is not in the table is `ENOSYS`, never a guess
at what it might have meant.

## The control commands of `ioctl`

The call has three arguments (`fd, cmd, arg`) and the commands are the
contract of the object behind the descriptor. The numbers are frozen like the
numbers of a system call: a new command takes the next free one.

| Command | Where | Meaning |
|---|---|---|
| `FBIO_TAKE` 0x4D01 | `/dev/fb0` | the console stops drawing; the picture is the program's |
| `FBIO_GIVE` 0x4D02 | `/dev/fb0` | the console repaints its grid and draws again; a give while another program holds the device changes nothing, so a program that starts a program keeps the picture |
| `TCGETS` 0x5401 | `/dev/tty0`, `/dev/tty`, 0, 1 and 2 | the mode of the terminal, into a `struct termios` of 36 bytes |
| `TCSETS` 0x5402 | the same | set the mode now; `TCSETSW` 0x5403 is the same and `TCSETSF` 0x5404 sets it and drops the input |
| `TIOCGPGRP` 0x540F | the same | the foreground process group of the tty, into a `k_u32_t` |
| `TIOCSPGRP` 0x5410 | the same | set the foreground process group of the tty from a `k_u32_t` |
| `TIOCGWINSZ` 0x5413 | the same | the size of the screen in cells, into a `struct winsize` of 8 bytes |
| `TIOCSWINSZ` 0x5414 | the same | accepted and ignored: nothing resizes a screen from a program |
| `FIONREAD` 0x541B | the same | the bytes that wait to be read, into an `int` |

The two structures, the flags and the indices of `c_cc` are the ones of Linux
and cross the call as they are (`kernel/tty.h`, ADR 0051 §3). The descriptors
0, 1 and 2 are the terminal of the task **while they are not open**, which is
the same rule `write` has had since 1.0 and the reason a program that was
written for a Unix finds a terminal in what it inherited:

| Descriptor | Not open | Open |
|---|---|---|
| 0 | `read` and `ioctl` of the terminal of the task | the object the program opened |
| 1, 2 | `write` to the console; `ioctl` of the terminal | the object the program opened |
| `/dev/tty0`, `/dev/tty` | — | the terminal of the task: the console of the machine, or its own for a session of telnet |

A panic always takes the screen back, whatever a program holds (ADR 0028). A
command that is not one of the table, or a descriptor whose object has no
control command, answers `EINVAL`.

The two devices of the interface are `read` and `write` documents of their
own: `/dev/fb0` reads a `struct fb_info` and writes a `struct fb_rect` followed
by `w * h` pixels of RGB565, and `/dev/input0` reads a `struct input_event`
with `EAGAIN` when the user is silent. The structures are in
`kernel/fb.h` and `kernel/input.h`, which a program includes as it includes
`kernel/ksys.h`.

The flags of `open` are the same as the ones of the VFS:

| Flag | Meaning |
|---|---|
| `KSYS_O_RDONLY` | only to read |
| `KSYS_O_WRONLY` | only to write |
| `KSYS_O_RDWR` | to read and to write |
| `KSYS_O_CREAT` | create the file if it is not there |
| `KSYS_O_TRUNC` | cut the file to zero |
| `KSYS_O_APPEND` | write at the end |
| `KSYS_O_DIRECTORY` | open a directory, for `readdir` |
| `KSYS_O_EXCL` | with `KSYS_O_CREAT`, refuse a name that is already there |

`KSYS_O_DIRECTORY` (`0x0800`) goes with `KSYS_O_RDONLY` and with no other
flag: a directory of this system is read, never written. The number is not the
one of Linux, whose flag does not fit the small set of this contract. The
descriptor of a directory is opened by `open` and read by `readdir`, and the
plain `open` of a directory without the flag is still `EISDIR`.

The `whence` of `lseek`: `KSYS_SEEK_SET` (from the beginning), `KSYS_SEEK_CUR`
(from the current position), `KSYS_SEEK_END` (from the end).

## The edge of the table

The table is the only door a program has, and a program is not trusted, so
every argument is checked where it enters - in `components/kernel/src/ksys.c`,
one small function per kind of argument. The checks are the same for every
call, and a call that breaks one is answered before a filesystem sees it.

**A path** (`path_check`) is not NULL, it begins at the root - every
filesystem of the tree hangs below `/` - and it ends inside `KSYS_PATH_MAX`
(`VFS_PATH_MAX - 1`, 127 characters). The search for the terminator stops at
the edge of that window, so the kernel never walks past the memory it is
willing to read. A path that is not a path is `EINVAL`; one that does not end
in the window is `ENAMETOOLONG` (`KERR_NAMETOOLONG`, the number 36 of POSIX).
`E2BIG` belongs to a buffer that is too large, not to a name that is too long.

**A buffer** (`buffer_check`) is NULL only with a zero length, and no call
touches more than `KSYS_MAX_IO` (65536 bytes) in one exchange. The number is
far above any buffer of the system and well below what could not be a real
transfer on a machine with ~100 KB of RAM. NULL with a length is `EINVAL`, a
length above the limit is `E2BIG`.

**The memory of the caller** is checked as well, through the port
(`k_port_exec_contains`, a call of the contract since 1.7). On the board, the simulator and the
null port a program shares the kernel's memory and every pointer is the
kernel's own; on the x86-64 port the kernel walks the page tables of the
program that made the call and refuses a range that is not there with
`EFAULT` (`KERR_FAULT`, POSIX 14). The null page, the guard page under the
stack, the part of the window that was never mapped and a range that runs past
`0x16000000` are all "not there". The kernel then copies the bytes with
`copy_from_user()`/`copy_to_user()`; a path is first checked byte by byte and
copied into a buffer of the kernel, so the VFS and the filesystems under it
never see a pointer of the program. A pointer that is not mapped is therefore
an error of the call and not a page fault of ring 0 that stops the machine
(ADR 0007).

**The flags of an `open`** (`open_flags_check`) are one of the three access
modes - both bits at once is not a mode - and no bit outside
`KSYS_O_ALLOWED`. The flags are checked before the file is looked for, so the
answer is about the argument and not about the object.

**The `whence` of `lseek`** and **the pid of `waitpid`** are compared before
a cast to `int` can cut a wide register down to a valid value: `2^32 + 1` is
not `SEEK_CUR`, and `2^32 + 5` is not the child 5. The `whence` is one of
three numbers; a pid below -1 names no task, and only 0 and -1 mean "any
child"; the flags of `waitpid` are one bit. All of them are `EINVAL`.

**`stat`, `statx` and `getcwd`** need the place for the answer: NULL there
is a mistake of the caller, not "the answer is not wanted", and it is
`EINVAL` instead of a question to a filesystem that has nowhere to put its
answer. A place that is not memory of the caller is `EFAULT`; the answer is
copied there through `copy_to_user()`, and the same holds for the status of
`waitpid`.

**`readdir`** takes whole entries: a count that is not a multiple of
`struct vfs_dirent` would let the last entry land past the memory of the
caller, so it is `EINVAL`. The entries come through the descriptor of a
directory that was opened with `KSYS_O_DIRECTORY`, and the position of the
next entry lives in that descriptor; the end of the directory is an empty
answer and not an error. A descriptor that is not a directory is `ENOTDIR`.
The shell still lists a path with `vfs_list`, which needs no descriptor.

## The mode, the owner and the group of an object

Every object has a mode, an owner and a group. The mode is nine bits of `rwx`
for the user, the group and the others, the owner is a uid and the group a
gid; a new file is `0644` and a new directory `0755`. `stat` reports all
three, in the `struct vfs_stat` it copies to the caller, and `chmod` (number
23), `chown` (number 24) and `chgrp` (number 51) change them one at a time.

Not every filesystem keeps all three. MicroFS keeps them in the record of a
version 3 volume (a volume of version 1 or 2 has no word for the group and
answers `ENOTSUP`), ext2 keeps them in the inode, FAT32 keeps none of them and
answers `ENOTSUP` to `chmod`, `chown` and `chgrp` alike, and a virtual
filesystem that keeps no modes - procfs, devfs - leaves the three at zero.

A name that a create makes (`open` with `O_CREAT`, `mkdir`) takes the uid of
the task that made it as its owner and the gid of that task as its group; the
record of a filesystem is born with the two of root because a filesystem has
no task, and the VFS is the layer that knows who is calling. An open of a name
that was already there moves neither number.

Every task has a **uid** and a **gid** (numbers 36 to 40 read and change
them). A task takes both from the task that created it; a fork, a program and
an `execve` keep them, so `getuid()` answers the identity the caller was
started with and not a constant. The root of the system is uid 0 (`KSYS_UID`):
the boot, the kernel itself and the shell of the console, until a person types
`login` or `su` (ADR 0033). The accounts are `/etc/passwd` (a name, a uid, a
gid, a home and a shell), `/etc/shadow` (a salted SHA-256 of a password) and
`/etc/group` (a name, a gid and a member list), read by the shell and by
`getpwnam`/`getpwuid`/`getgrgid`/`getgrnam` of the library - the same files
for the two, so a program and the console cannot disagree about a name.

* a task whose uid is the owner of an object reads the triad of the owner; a
  task of another uid whose gid is the group of the object reads the triad of
  the group; every other task reads the triad of the others. An object whose
  filesystem keeps no group answers zero, which is the group of root, so the
  triad of the group is never the answer of a task that is not root - the
  answer of a filesystem that has no groups at all;
* **root passes the read and the write triads.** It is the exemption of POSIX:
  a file of the mode `0200` is read by root, and one of `0400` is written by
  it. The exception is the execute bit, which `execve` checks itself: root
  runs a file that has an `x` bit for somebody, not a file with no `x` bit at
  all. A zeroed mode still means "this filesystem keeps no modes".

`open` compares the access in its flags with the mode of the object before a
descriptor exists, so every filesystem under the VFS answers a forbidden open
the same way - `EPERM`. A file that is not there is not checked: with
`O_CREAT` it is about to be made with the mode of the design, and without
`O_CREAT` the open itself answers `ENOENT`. A zeroed mode is not "nobody may
touch it": it means "this filesystem keeps no modes" - `procfs` and `devfs` -
and those objects keep the access they always had.

`chmod` belongs to the owner of the object and to root, `chown` to root
alone, `chgrp` to root, or to the owner of the object giving it to the group
of its own task (a task has one gid and no supplementary list, ADR 0044), and
`mount`/`umount` to root alone; a task of another uid learns `EPERM`.

A task has the **three uids of POSIX** (ADR 0053): the real uid is the
person, the effective uid is the identity every check of the kernel reads -
`geteuid` answers it and `getuid` answers the real one - and the saved uid is
the identity `setuid` may return to. `login` and `su` move the three together
(`task_set_creds`, the gid first), and every program the person starts
inherits them.

`setuid` follows the rule of POSIX with these three: a task whose effective
uid is zero names any uid of the fifteen bits of a record and the three become
it (which is how a program that entered as root gives the identity up with
`setuid(getuid())`, the saved uid included); any other task names its real or
its saved uid and the effective uid moves to it, and any other number is
`EPERM`. `setgid` keeps the shape of one gid: root names any group of the
fifteen bits, a task names the one it has.

An image may ask for the identity of the owner of its file with the
`K_EXEC_FLAG_SETUID` flag of the header (ADR 0053): `execve` then makes the
effective and the saved uid of the caller the owner of the file, for as long as
the program runs, and refuses the image with `EPERM` when the caller may write
the file (root is exempt). The bit is not a mode and `chmod` cannot grant it;
`pkg` installs such an image only as root.

The mode of `chmod` is refused outside the nine bits (`EINVAL`), and so are an
owner outside the fifteen bits of a uid and a group outside the fifteen bits
of a gid (`EINVAL`): a number that does not fit would spill into the type of
the object. `chmod 000` is not expressible
either: a zero in the record means "the record predates the modes", so the
filesystem refuses it instead of storing a number it would read back as
`0644`. `stat` checks nothing and reports the three numbers to whoever asks.

The **`x` bit of every directory of a path** is checked before the object at
the end is looked at. The VFS walks the components of the path, calls `stat`
on each directory on the way and refuses the walk with `EPERM` when the caller
may not search through it: a directory of the mode `0600` keeps its children
out of reach for `open`, `stat`, `execve` and every other call that resolves a
path, and one of `0700` lets them through. Root passes the search of every
directory, like the read and the write triads. The directory that is named at
the end is not searched through - `stat` of a `0600` directory itself still
works, and opening it as a directory needs the read bit, not the search bit.
A zeroed mode is still "this filesystem keeps no modes", so the roots of
`procfs` and `devfs` are walked the way they always were.

## The moments of a file

`stat` has no field for a time or a group, and its structure and its number
are frozen. `statx` (number 26) is the second door: the first five fields of
`struct vfs_statx` are the five fields of `struct vfs_stat`, in the same
order, the sixth is the group of the object (ADR 0038) and the two last are
the moments of the object.

```c
struct vfs_statx {
    k_u32_t inode;
    k_u8_t  type;
    k_u64_t size;
    k_u32_t mode;
    k_u32_t owner;
    k_u64_t mtime; /* the last change of the content */
    k_u64_t ctime; /* the last change of the record, the mode or the owner */
};
```

**The group of a record arrived in 1.7** (ADR 0038) in the room that the
alignment of `type` left free: `group` is a 16-bit field at the offset 6, and
the structure keeps the 40 bytes it has had since the call was frozen. A
program of an older tree reads the fields it knows and this one reads the
group as well, so the epoch is not broken and an image of 1.0 still runs (the
test of the frozen image holds it). The group is zero for every filesystem
that has no group of its own - all of them but a MicroFS volume of version 3
- and zero is the group of root, which is what a volume without a group has
always meant.

The moments are the seconds of Unix in 64 bits, so the year 2038 is not a
wall of the contract. They come from the clock of the kernel
(`k_clock_now_unix()`): the process of the host, the CMOS of an x86-64 machine,
the BM8563 of the board and the NTP service all feed the same number (see "The
clock of a machine"). A machine whose chip has no battery and whose network is
down stamps with the seconds since its boot, which orders the changes but is
not a date; `k_clock_present()` says which one it is.

**Zero means "this filesystem keeps no time".** A volume of MicroFS of
version 1 has 16-byte records with no room for a moment, and a filesystem
like procfs, devfs or the FAT reader has no moment of its own; both answer
zero for the two fields. It is not the first second of 1970, and a caller
must not print it as a date. MicroFS of version 2 (ADR 0013) moves `mtime`
on create, write and truncate, and `ctime` on those and on `chmod`/`chown`.
There is no access time: a read does not rewrite a record, so `statx` has no
field that would always be zero.

The library has the same call as `microos_statx()` and `struct
microos_statx`, with the types of `<stdint.h>`; it returns -1 and sets
`microos_errno()` on an error, like the rest of the library.

## The clock of a machine

The call 49, `clock_gettime(clock, buf)`, is the clock of POSIX in the shape
of this system: the clock is one of the two selectors, the answer is written
into the place of the caller and the wire is two signed 64-bit words on both
machines, because the seconds of Unix are the same number on the board and on
x86-64. A 32-bit program translates them into its own `struct timespec` in the
library of a program.

```c
struct ksys_timespec {
    k_i64_t tv_sec;  /* seconds; since 1970 for CLOCK_REALTIME */
    k_i64_t tv_nsec; /* the fraction of a second, 0 to 999999999 */
};

#define KSYS_CLOCK_REALTIME 0  /* the wall clock */
#define KSYS_CLOCK_MONOTONIC 1 /* the milliseconds of the boot, as uptime */
```

`CLOCK_REALTIME` is the wall clock of the kernel (`k_clock_now_unix()`, the
one `statx` stamps a record with). **Who sets it: the machine at its boot, and
then the network or a person.** The port and the drivers register a source
when they come up and read it once: the host the process it runs on, the
x86-64 machine the CMOS chip of the firmware (ports 0x70 and 0x71, BCD or
binary, 12 or 24 hours, read twice to avoid the update of the chip), the
board the BM8563 behind I2C. `ntp` then corrects the moment and `date set`
writes a new one on the board, and both go through `k_clock_set_unix()`, which
moves the base the kernel adds the uptime to *and* asks the driver to write
the chip, so the next boot starts from the corrected time. A machine whose
chip has no battery and whose network is down answers the seconds since its
boot: a number that orders two records, and not a date (the zone of `/etc` is
still a debt of `docs/debts.md`).

`CLOCK_MONOTONIC` is the millisecond count of the boot that `uptime` answers;
a caller that measures an interval uses it, so a step of `ntp` does not make a
timer jump. Any other selector, and a place that is not there, is `EINVAL`.

## The use of the processor

`getrusage(buf)` (number 50) is the processor time of the calling task - the
number the scheduler already counts for the CPU limit of the task
(`KSYS_RLIMIT_CPU`, kernel/task.h) - brought up to the moment of the call and
copied into the place of the caller:

```c
struct ksys_rusage {
    k_u64_t utime_ms; /* the time the task spent running */
    k_u64_t stime_ms; /* the part of it the kernel spent for the task */
};
```

This system makes no boundary between the two on a machine of one privilege
level: a call of the board is a function call and the time of the kernel
inside a call of x86-64 is the time of the caller. The whole of the count is
the user time and `stime_ms` is zero, which is a fact about the machine and
not a field that was forgotten. `RUSAGE_CHILDREN` of POSIX has no call: the
usage of a child that has ended is not kept, and the library answers `ENOSYS`
for it rather than a zero that looks like a number.

## The directory of a task

Every task has one working directory, a path of at most `TASK_CWD_MAX` (127)
characters in its record. The shell's `cd` writes it (`task_set_cwd`), a
program writes it with the call 48 `chdir(path)`, and `fork` and `execve`
carry it: the child of a fork is a copy of the record and the image of a
program starts in the directory of the task that entered it. The call resolves
the name through the VFS, refuses a name that is not a directory with
`ENOTDIR` and a name that does not fit the record with `ENAMETOOLONG` - it is
not cut to fit - and leaves the directory the task had when it refuses.

A relative path is still resolved by the library of a program before the call:
the VFS of this system mounts every filesystem below `/` and its calls take a
path that begins there (`path_check`). The library asks the kernel for the
directory of the task (`getcwd`) for every relative name and folds `.` and
`..`, so the two sides cannot disagree about where the program is. On the
board a program uses the paths of the shell and the resolver is not linked.

## The programs

`execve(path, argv, envp)` makes the caller a program:

* the file is read through the VFS, not through a descriptor of the caller,
  so a failed call leaves the table of the task exactly as it was;
* a program is at least the 32 bytes of the header of `kernel/exec.h` and at
  most `K_EXEC_MAX_IMAGE` (8 MiB, the memory of the machine). The file is
  opened and its size is checked before a byte is read: `E2BIG` past the
  ceiling and `EINVAL` below the header. On a machine with page tables only
  the header is read; the text, the data and the bss become three regions of
  the address space and the pages come from the file when the program touches
  them (`kernel/mm.h`), so a program of 4 MiB costs its own pages and not a
  copy of itself in the memory of the kernel. A machine without page tables
  reads the whole file into one block in pieces of `KSYS_MAX_IO` (64 KiB), so
  the transfer edge of a call is not the limit of the file;
* the image must be a `MOX1` of version 2 and of a machine the format knows;
  the arch of the header is then compared with `k_port_exec_arch()`, and an
  image of another machine is `ENOEXEC` before a byte of it is entered;
* the port is then asked whether the machine can run a program at all. The
  board and the simulator answer no, and the answer of the call is `ENOSYS`;
  the x86-64 port names a window of memory, builds the page tables of the
  program, copies the image into the physical arena behind the window and
  enters it through an `iretq` into ring 3 (ADR 0007).

The errors: `ENOENT` for a path that names nothing, `EISDIR` for a directory,
`EPERM` for a file whose mode has no execute bit for the caller (`0400` is not
a program), `E2BIG` (`KERR_TOOBIG`, POSIX 7) for an image past
`K_EXEC_MAX_IMAGE`, `EINVAL` for a file that is not an image of version 2 of
this system, `ENOEXEC` (`KERR_NOEXEC`, POSIX 8) for an image of another
machine, `ENOMEM` for an image the port could not place in its window, and
`ENOSYS` for a machine without a window of programs.

A successful call **does not return** when a program of ring 3 makes it: the
image replaces the memory of the caller and the entry of the program is the
return of the call (the port rewrites the frame of the system call and the
`iretq` of the stub goes to the new entry). The task keeps its pid, its
parent, its descriptors and its directory - POSIX - so the status the program
ends with is the answer of the `waitpid` of its parent, not of this call.

A caller with no address space is a task of the kernel - the shell - and there
is nothing to replace: the port starts the program as a task of its own and
the call returns when the program has ended, with 0 and not the status of the
program. The status is recorded in the calling kernel task for the shell's
pipeline and list arithmetic; `waitpid` of the program's parent sees the same
code. This is the compatibility shape of a machine whose shell is not a
program; the board keeps it for every caller.

The vector of the arguments is the memory of the program. It is copied word
by word through `copy_from_user()`; on a machine with paging a word or a byte
that is not the memory of the caller is `EFAULT`, and a vector that is too
long or a string that does not end in the frame is `E2BIG`. At most eight
words and 512 bytes of their strings are copied into the frame of the call,
and on a machine with paging the port copies them again into the memory of
the program. The kernel therefore hands the port a vector it built, not the
pointers of the caller. A line of the shell is `exec /bin/hello one two`:
three words, the name of the program first, as in Linux. The x86-64 port also
places a zero at the top of the stack, so a program that returns instead of
exiting faults on address zero.

### The environment

`envp` is the third argument of the call: a vector of "NAME=VALUE" words
terminated by NULL, copied into a frame of its own by the same code that
copies the arguments and bounded the same way (eight words, 512 bytes). A
NULL vector is **the environment of the system** and not an empty one: the
kernel fills it with `k_exec_environ()`, which today is four words -
`PATH=/bin`, `HOME=/`, `USER=root`, `PWD=/` - because the shell has no
`export` yet. A vector whose first word is NULL is an empty environment, and
the program sees `envp[0] == NULL`.

The environment travels to the port in `struct k_exec_image.envp`, next to the
bytes of the image; the port contract (1.6) does not change. `execve` answers
the same errors for a bad environment as for a bad vector of arguments, before
the file is read: a refused call spends nothing.

The library of a program (`components/libc/user`) writes the entry as
`_start(argc, argv, envp)`, calls `main(argc, argv, envp)` and then `exit`.
`components/libc` (the library of the kernel) keeps `microos_execve(path,
argv)` for the callers that pass no environment; `ksys_execve_env()` is the
wrapper of the three-argument call.

## The processes

A `fork` on a machine without an MMU cannot copy the memory of the parent:
the tasks **share** it, like the flag `CLONE_VM` of Linux. The child gets its
own number, and the program tells the child by the mark of its task
(`task_self()->is_child`) - in Linux the child knows it by the zero that
`fork()` returns, here the child starts the program from the beginning.

On the x86-64 port the call is real (K4, contract 1.5): a program is a task
with an address space of its own, `fork()` copies the page tables, both
copies share the frames read-only, and the first write of either side faults
and gets a page of its own. The child returns from the same call with 0 and
the parent with the pid, as POSIX promises, and `waitpid()` sees the child
the usual way. The port does the copy (`k_port_fork_copy()`); the kernel
still decides the pid, the parent and the wait. The clone flushes the TLB
entry of every page it makes read-only, and the parent's first write after the
fork is the write that copies it, not a write into the frame the child is
about to use.

`execve()` from a program is the other half of the pair: the image replaces
the caller, not a task beside it, and the descriptors stay open (POSIX). A
child of a `fork` that calls `execvp()` keeps its pid and becomes the new
program in place, and the parent reaps it with `waitpid()` and gets the status
of that program.

`waitpid()` waits for a child, the status is the code the child finished
with. The number 0 or -1 means "any child": the pid travels in an unsigned
register, where -1 is larger than every pid, so the scan compares the signed
value and a parent that asks for any child finds the one it has. A call with
no child to wait for is `ECHILD` (`KERR_CHILD`, the number 10 of POSIX),
whether it was ready to wait or not, and the same answer ends the waiting once
the internal limit of five seconds passes without a finished child. A number
that names no child of the caller is `ECHILD` too; a pid or a flag that is not
of the contract is `EINVAL` before that.

## The signals

Four signals are in the ABI: `KSYS_SIGINT` (2), `KSYS_SIGPIPE` (13),
`KSYS_SIGTERM` (15) and `KSYS_SIGCHLD` (17). The numbers are the ones of
POSIX and the names of `kernel/ksys.h`; a number outside the four is
`EINVAL`. The dispositions are a pointer of the caller or the two values of
POSIX, `KSYS_SIG_DFL` (0) and `KSYS_SIG_IGN` (1); `signal()` answers the
previous disposition as the raw value of a register, so a zero and a one
are the two kernel dispositions and not handler addresses.

`kill(pid, sig)` accepts a positive pid and one of the four signals. The
system has no process groups: 0 and -1 are `EINVAL`, not "the group" and
"everybody". A pid that names no live task is `ESRCH`; the number is `-3`,
the same as `KERR_EXIST`, because `-3` already meant "file exists" in the
frozen table (ADR 0015), and the shell prints the sentence of the signal
rather than the text of the file error. A task of another uid is not a target
of a task that is not root: the call answers `EPERM`, and root may end any
task.

A signal is answered in the context of the task that receives it: on the way
out of a system call, or on the tick of the timer when the interrupted code
is a program of ring 3. The default action of `SIGINT`, `SIGTERM` and
`SIGPIPE` is the end of the task with the status `128 + sig`; the default of
`SIGCHLD` is to be ignored. A handler runs before the interrupted call
returns its answer; on x86-64 it is a function of the program in ring 3,
entered through a frame on the stack of the program and returned from
through `sigreturn` (29), which is a call of the port and not of a program.

`waitpid()` scans a finished child before the pending signals, so the
`SIGCHLD` of a child that has just ended does not turn the wait of its
status into an error. A pending signal that has to be answered - the
default end, or a handler the task installed - makes the call answer
`EINTR` (`KERR_INTR`, -4) without reaping the child; `ECHILD` keeps its
meaning. An ignored signal and the default `SIGCHLD` do not cut a wait.

SIGSTOP (19), SIGTSTP (20) and SIGCONT (18) are scheduler operations rather
than pending marks. STOP and TSTP remove a living task from every run queue
and from the sleep table; CONT is the only operation that makes it ready
again. They cannot be caught or ignored. Ctrl+C at the console sends SIGINT
to the foreground process group, and Ctrl+Z sends SIGTSTP to it. The shell
turns that group into a numbered job, restores its own foreground group while
it reads the next command, and continues the job with `fg` or `bg`.

The library has the same calls as `microos_kill()` and
`microos_signal()`, with `MICROOS_SIGINT`, `MICROOS_SIGCONT`,
`MICROOS_SIGSTOP`, `MICROOS_SIGTSTP` and the other POSIX numbers, and the two
dispositions `MICROOS_SIG_DFL` and `MICROOS_SIG_IGN`;
`microos_signal()` answers `MICROOS_SIG_ERR` and sets `microos_errno()`
when the signal is not one of the marks with a disposition.

## The memory of a program

Four calls give a program the memory it asks for: `mmap`, `munmap`, `msync`
and `brk`. None of them reads a file: a mapping is a record of the address
space (`kernel/mm.h`) and the page behind an address is made, filled and
mapped the moment the program touches it. A program that maps a file of
64 MiB and reads one page of it costs one page, and a program that starts
from a file of 4 MiB costs the pages it runs.

```c
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t len);
int   msync(void *addr, size_t len);
```

The protection is `KSYS_PROT_READ` (0x1), `KSYS_PROT_WRITE` (0x2) and
`KSYS_PROT_EXEC` (0x4), and the flags are `KSYS_MAP_SHARED` (0x1),
`KSYS_MAP_PRIVATE` (0x2) and `KSYS_MAP_ANONYMOUS` (0x4). One of the two
sharing flags is always set, and a mapping is either of a file or of no file.
`addr` is a hint: NULL asks for any free range, and an address that is taken
is ignored rather than refused, because the system has no `MAP_FIXED`.
`len` and `offset` are whole numbers of `KSYS_PAGE_SIZE` (4096) and the offset
is into the file behind `fd`.

The policy of a page is one rule of the ABI since 2.0 (ADR 0052): a page is
code or it is data, never both. `mmap` with `KSYS_PROT_WRITE` and
`KSYS_PROT_EXEC` together is `EINVAL` before a page is made, and a program
that asks for it keeps running. The other half is the ceiling of the task:
the bytes of an anonymous mapping are counted against
`KSYS_RLIMIT_MEMORY` (`ulimit -m`) as the heap is, and a mapping past it is
`ENOMEM`.

The answers and the errors:

* `mmap` answers the address of the range, `EINVAL` for a length or an offset
  that is not a page, a `prot` or a flag outside the set, neither or both of
  the sharing flags, a `prot` that is writable and executable at once, and an
  address that is not aligned; `EBADF` for a descriptor that is not open;
  `ESPIPE` for a pipe (it has no bytes to map); `ENOMEM` for a range that does
  not fit the window of the program or the memory limit of the task;
  `ENOSYS` on a machine without page tables.
* `munmap` answers 0 and gives the pages back; the shared pages of the range
  go to their file first. A range that cuts a mapping in two is `EINVAL`
  rather than a silent half; a range that holds no mapping is 0 and does
  nothing, as POSIX has it.
* `msync` writes the pages of the shared mappings of a range that were
  written back to their file. A private mapping has nothing to write.
* `brk(addr)` moves the end of the heap and answers the new end; `brk(NULL)`
  answers the current one. The heap begins after the image of the program and
  may not cross a mapping, the top of the window, where the restorer, the
  guard page and the room of the stack are, or the memory limit of the task:
  the refusal is `ENOMEM` and the break stays where it was. A heap that
  shrinks gives the pages above the new end back.

The two sharing modes are the two answers of a write. A write into a
`KSYS_MAP_PRIVATE` page is a copy of that page for this program alone, and the
file keeps its bytes. A write into a `KSYS_MAP_SHARED` page is a write of the
file: the page is marked dirty and `msync`, `munmap` or the end of the program
writes it back through the VFS, so the next reader of the file - `cat`, the
shell, `e2fsck` on the volume of an ext2 root - sees the bytes the program
wrote. A page that was only read is never written back.

The page of an image is the same machinery: `execve` maps the text (readable
and executable), the data (readable and writable) and the bss (zeros) as
private regions of the file, so a write of the data of a program copies that
page and the file on the volume is never changed by a running program.

## The wait for a descriptor


`poll(fds, nfds, timeout)` (number 43) waits for one of the descriptors of the
list to be ready. An entry is eight bytes on both machines and the library
uses the structure of the ABI as it is:

```c
struct ksys_pollfd {
    k_u32_t fd;      /* a negative number is ignored */
    k_u16_t events;  /* KSYS_POLLIN, KSYS_POLLOUT */
    k_u16_t revents; /* what is true, written by the call */
};
```

The bits are the ones of Linux: `KSYS_POLLIN` (0x0001) for a read that would
not wait, `KSYS_POLLOUT` (0x0004) for a write that would not wait, and
`KSYS_POLLERR` (0x0008) for an object that is gone - the writing end of a
pipe whose reader closed, a descriptor the caller may not use in the direction
it asked, and a number that is not open. An entry the caller did not ask about
stays zero; the error is reported whether it was asked for or not, because a
wait that would never end is not an answer.

The answers:

* the count of the entries whose `revents` is not zero, the list written back
  to the memory of the caller;
* 0 when the timeout passed, and for a list of no descriptor - `poll(NULL, 0,
  ms)` is the sleep of the system, so a program of the console can wait
  without a descriptor;
* `EINTR` when a signal the task has to answer cut the wait;
* `EINVAL` for a list longer than `KSYS_POLL_MAX` (16, the table of the
  descriptors of a task), a NULL list with a count, or a timeout that is not
  -1 and not a number of milliseconds an int holds;
* `EFAULT` for a list that is not the memory of the caller.

A timeout of -1 waits without a deadline. The wait is the scheduler's and not
a loop of the task: the task is taken out of the run queue and the object
wakes it - the writing end of a pipe after a byte, the reading end after the
room it made, the close of either end, the console when a key arrives. A
machine whose tasks are threads of another scheduler (the host, the board)
cannot cut a sleep from outside the thread, so its wait looks at the state
every 10 ms; the x86-64 port answers within the tick of the timer.

The system has no socket in the ABI, so a descriptor of `poll` is a file, a
pipe or a device of the VFS. A regular file is always ready to be read and to
be written, and `/dev/tty0` is readable when a character is waiting.

## The descriptors

Every task has its own table of the open files, sixteen of them. The
descriptor is the number of the slot. A descriptor that was never opened
gives `KERR_BADF`. The table is the ceiling of the machine; the task also has
a limit of its own (`KSYS_RLIMIT_FDS`, below), which starts at sixteen and may
be lowered, so a task may refuse itself a descriptor that the table would
still hold. Sixteen slots hold the database file and the journal of SQLite,
the stages of a pipeline with their redirects, and the files `xargs` reads at
once.

The open file behind a descriptor is a **description** of the kernel, counted
by the descriptors that refer to it (`kernel/kfile.h`). One task gets two
descriptors of one description from `dup2`, and the parent and the child of a
fork each get one, so the position of a file and the ends of a pipe are one
state: closing a descriptor does not close the others, and a write through one
descriptor continues where the other stopped. A description lives in a fixed
table of 24 records and is given back by the last close. A task that exits
closes its descriptors.

A descriptor remembers the access it was opened with - read, write, or both.
The kernel checks that access at the edge of the system, so every filesystem
under it answers the same way: reading a descriptor that was opened to be
written to, or writing one that was opened to be read from, gives
`KERR_BADF`. The mode of the object is a second, earlier check (see "The mode
and the owner of an object"): an open that the mode does not allow never
becomes a descriptor at all, and the answer is `EPERM`.

A descriptor of a directory is opened with `KSYS_O_DIRECTORY` and read with
`readdir`; it has no other use. It is opened for reading only, so a write
never becomes a descriptor of a directory at all, and `read` of it answers
`EISDIR`. `readdir` of any other descriptor answers `ENOTDIR`.

## The cut of a file

`ftruncate(fd, size)` (number 35) cuts the open file behind the descriptor to
`size` bytes. A shorter file gives the blocks of its tail back to the map of
the free blocks, and the bytes past the new size of the last block are zeroed;
a longer file is filled with zeros to the new size. The position of the
descriptor does not move, as POSIX has it, even when it is past the new size.

The descriptor has to be open for writing: a descriptor of a reader is
`EBADF`, and so is a number that is not open. A pipe is `EINVAL` - it is a
ring of bytes and not a file with a size - and a descriptor of a directory is
`EISDIR` before the filesystem is asked. The size is a 64-bit register on the
host and on x86-64 and a 32-bit one on the board; a number wider than the
record of the filesystem is `E2BIG`, and a growth the volume cannot hold is
`ENOSPC`.

MicroFS and ext2 implement the operation. FAT32 answers `KERR_NOTSUP` for
now, and so do procfs and devfs: the call is honest about what the volume can
do instead of pretending that a cut happened. The library of a program has
`int ftruncate(int fd, long long size)` in `components/libc/user`, in the
shape of POSIX (0 or -1 with `errno`).

## The pipes and `dup2`

`pipe(fds)` (number 27) makes a pipe and gives the caller two ordinary
descriptors: `fds[0]` reads and `fds[1]` writes. The two are the first free
slots of the task, so a program that has closed its console gets 0 and 1.
The pipe is a ring of the port (64 KiB on x86-64 and the host, 4 KiB on the
board, ADR 0035) taken from the allocator with the pipe and given back with
its last end; the system keeps sixteen of them. The place for the answer is
checked at the edge: NULL is `EINVAL`, a place that is not the memory of the
caller is `EFAULT`.

* A read of an empty pipe whose writing end is still open sleeps until the
  bytes arrive: a reader slower than its writer is an ordinary pipeline and
  not an error. A signal that has to be answered cuts the wait short and the
  answer is `EINTR`. A read of an empty pipe with no writer is 0, the end of
  the file.
* A write with no reading end is `EPIPE` (`KERR_PIPE`, the number 32 of
  POSIX). There is no `SIGPIPE` in this system yet, so the error is the whole
  answer; the bytes written before the reader went away are the count.
* A write of at most `PIPE_BUF` (512) bytes waits for the whole of its room
  and lands as one step of the ring, so two writers of a pipeline cannot put
  a byte inside one block of each other. A larger write is placed as the
  reader makes room.
* The access of each end is the flag of its descriptor: `read` of the writing
  end and `write` of the reading end are `EBADF`. A pipe has no position, so
  `lseek` is `ESPIPE` (`KERR_SPIPE`, 29), and it has no entries, so `readdir`
  is `ENOTDIR`.
* A wait of a pipe is the scheduler's (`kernel/wait.h`) and has no bound: a
  reader slower than its writer is an ordinary pipeline and not an error. The
  task leaves the run queue, and the other end wakes it when it puts a byte in,
  takes one out or closes; a signal cuts the wait with `EINTR`. The ring is
  `KFILE_PIPE_SIZE` bytes - 64 KiB on x86-64 and the host, 4 KiB on the board -
  taken from the allocator when the pipe is made.

`dup2(oldfd, newfd)` (number 28) makes `newfd` a second descriptor of the
open file of `oldfd` and answers `newfd`. A descriptor that was open at
`newfd` is closed by the call, and `oldfd == newfd` changes nothing. A
number outside the table of the task, or a descriptor that is not open, is
`EBADF`. Because the two descriptors name one description, the copy of a
write to a file continues at the position of the original, and a duplicated
end of a pipe keeps the pipe open until the last of its descriptors is
closed.

A fork copies every descriptor of the parent with one more reference each,
before the child can run, on the real copy-on-write fork of x86-64 and on the
placeholder of a machine without an MMU alike.

## The limits of a task

A task has three limits: the memory it may hold, the processor time it may
spend and the descriptors it may open. They belong to the task and to nobody
else; there is no call that changes the limit of another task, and the three
numbers are the task's own, not a budget of a user.

```c
int ksys_getlimit(k_u32_t which, k_u64_t *value);
int ksys_setlimit(k_u32_t which, k_u64_t value);
```

`which` names one resource; the value is a 64-bit word, so the three widths
of the sources (bytes, milliseconds, a count of descriptors) travel without a
structure. The library composes `struct microos_rlimit` from three calls for
a caller that wants the set in one place.

| Selector | Resource | Unit | Ceiling |
|---|---|---|---|
| `KSYS_RLIMIT_MEMORY` (1) | the user blocks the task holds | bytes | the machine: 2 MiB on the board and the host, the window of the programs on x86-64 (99 540 992 bytes, ADR 0057) |
| `KSYS_RLIMIT_CPU` (2) | the time the task spent running | milliseconds | 4294967295 (about 49 days) |
| `KSYS_RLIMIT_FDS` (3) | the descriptors the task may open | descriptors | `TASK_MAX_FDS` (16) |

**The policy: the ceiling is the start and a limit only goes down.** A fresh
task starts at the ceiling; `setlimit` accepts a value at or below the current
limit of the resource and refuses a raise with `EPERM`. The ceiling belongs to
the machine and even root does not lift it: a program that could raise its own
limit would have no limit at all. A selector outside the three is `EINVAL`; a value above
the ceiling of the machine is `EPERM` as well, because it is above the current
limit too. A child of a fork inherits the limits of its parent; a task made by
`task_create` starts at the ceiling.

`getlimit` copies the current limit into the memory of the caller through the
edge: NULL or a place that is not there is `EINVAL`/`EFAULT`. `setlimit` takes
the value in a register and has no pointer to check.

**The memory** is the sum of the blocks a task allocated with the priority of
a user (`KMM_PRIORITY_USER`, which is what `malloc` of a task uses) and has
not freed. The kernel and the system utilities are not charged: their
allocations have no owner, because the kernel must not fail for the budget of
an application. A block that would take the task over the limit makes `malloc`
and `realloc` answer 0, and the library turns that into `ENOMEM`
(`microos_malloc`). The blocks that are already held are not taken away when
the limit is lowered: the next allocation fails and `free` returns the room.
`realloc` asks for the new block before it gives the old one back, so a budget
that would only hold the result can refuse the call; the caller keeps its old
block, as POSIX promises. A block that outlives the task that allocated it is
not charged to anybody, because the kernel keeps no list of the blocks of a
task yet (see [debts.md](debts.md)).

**The CPU time** is counted in the milliseconds the task was running, at the
edge of every system call and at the tick of a port that can name the running
task; a sleep and a block set the accounting point again on both sides, so a
wait costs nothing. When the task reaches the limit it is marked with
`SIGTERM` once - the system has four signals and no `SIGXCPU`, and a fifth is
not in the contract. The default action of `SIGTERM` ends the task with the
status 143; a handler may catch it and the task goes on, as a handler of
`SIGXCPU` may. A task that never makes a system call is marked at the next
one; on x86-64 the tick of the timer delivers the mark to a program of ring 3
without a call.

**The descriptors**: `open`, `pipe` and `dup2` stop at the limit with
`EMFILE` (`KERR_MFILE`, 24). `dup2` refuses a `newfd` at or above the limit;
`dup2(fd, fd)` of a descriptor that is already open is still allowed, because
lowering a limit does not close what is open. A descriptor above a lowered
limit stays usable and may be closed; a pipe needs two free slots below the
limit. The table of the task is sixteen slots and the table of the
descriptions of the system is 24 records: a full limit is `EMFILE`, a full
system is `ENOSPC`.

A host test covers the three resources (`test_ksys.c`, `test_task.c`,
`test_kmm.c` and `test_libc.c`), and the shell has `ulimit` to print the limits
of the shell and to lower them.

## The wrappers

The programs use the wrappers, they check the arguments before they reach the
kernel:

```c
int fd = ksys_open("/etc/config", KSYS_O_RDONLY);
char buffer[256];
int got = ksys_read(fd, buffer, sizeof(buffer));
ksys_close(fd);
```

The wrappers are thin: they only pass the arguments on.

The library of the system (`components/libc`) has the same calls under the
names of POSIX: `microos_chmod`, `microos_chown`, `microos_statx`,
`microos_execve`, `microos_pipe` and `microos_dup2` (and the usual `chmod`,
`chown`, `statx`, `execve`, `pipe`, `dup2` when its header is included). They
return -1 and set `microos_errno()` when something goes wrong, and they
refuse the two numbers of a mode and of an owner that a record cannot hold
before the kernel is asked. The wrapper of the call itself is
`ksys_execve(path, argv)` for a caller without an environment and
`ksys_execve_env(path, argv, envp)` for one with it. The credentials have the
wrappers `ksys_getuid`, `ksys_geteuid`, `ksys_getgid`, `ksys_setuid` and
`ksys_setgid`, and the library of a program answers `getuid`, `geteuid`,
`getgid`, `setuid` and `setgid` through the same door
(`components/libc/user/src/pwd.c`).

The limits have the same shape: `microos_getlimit(which, &value)` and
`microos_setlimit(which, value)` with the selectors `MICROOS_RLIMIT_MEMORY`,
`MICROOS_RLIMIT_CPU` and `MICROOS_RLIMIT_FDS`, and `microos_getrlimits()` for
the three numbers at once. A raise is -1 with `EPERM` in `microos_errno()`,
and `microos_strerror()` names `EMFILE`, `ENOMEM` and the rest of the codes
the library can meet.

## The tests

The tests of the ABI do not check that "something" works, they check the
**properties**: the same data has to give the same result no matter how the
program breaks it into pieces.

* **The golden record.** The table of this document and the numbers around it
  are recorded in [tests/corpus/abi.txt](../tests/corpus/abi.txt), and the test
  `ksys: the ABI is the recorded one` reads the file and compares it with the
  running kernel: the calls (number, name, arity and result), the errors, the
  flags, the limits of the edge, the header of a program image and the size
  and the offsets of `vfs_stat`, `vfs_statx`, `vfs_dirent` and
  `k_exec_image`, for a 64-bit and for a 32-bit machine. A number, a name, a
  flag or a field that changes without the file is a failed test, and a line
  of the file without a change of the kernel is one too; the two are one
  commit. The arity and the result are documentation of the signature that the
  compiler checks through the prototypes of `kernel/ksys.h`.
* **The frozen image.** A MOX1 image of version 2, built for 1.0, is recorded
  in [tests/corpus/mox1](../tests/corpus/mox1) and parsed on every
  `make test` (`exec: the image of 1.0 is read as it was frozen`): the fields
  of the header, the sum of the bytes and the rule of the port that the text
  ends on a page. An image built for 1.0 is read by every 1.x.
* **The round trip.** The corpus of the numbers is written through the system
  calls in pieces of 1, 3, 7, 64, 512, 1024 and 4096 bytes. Every time the
  same bytes have to come back, and their sum has to be the same.
* **The alignment.** The same data, written from a buffer that is aligned and
  from one that is one byte further, gives the same result.
* **The numbers of the calls.** The table above is repeated in the test, so
  that a change of the contract is noticed at once; the table is also counted,
  so a number that is left empty is a failure and not a call that is missing.
* **The errors.** A file that does not exist, a descriptor that was never
  opened: the same error, no matter how the call was made.
* **The agreement.** The file that the system calls write is the file that the
  filesystem sees.
* **The mode.** A `chmod` is seen by the next `stat`, the triad of the owner
  decides for a task whose uid is the owner and the triad of the others for
  every other task, root passes the read and the write, and a mode that the
  record cannot hold is refused instead of stored. A directory whose mode has
  no search bit keeps its children out of reach, and one with the bit lets
  them through. A task of another uid is a real actor of the test: the
  runner moves itself with `task_set_uid` and back. `chown`, `mount` and
  `umount` answer `EPERM` to it, and so does a signal to a task of another
  uid.
* **The moments.** A create, a write and a truncate move `mtime` and `ctime`;
  a `chmod` moves `ctime` and leaves `mtime`; a remount keeps both; a volume
  of version 1 answers zero, which is "unknown" and not 1970. `statx` reports
  the same five numbers as `stat` and the two moments after them.
* **The directory.** A directory is opened with `O_DIRECTORY`, its entries
  come through `readdir` one call after another, the end of it is an empty
  answer, and a descriptor of a file is `ENOTDIR` to the same call.
* **The pipes.** The bytes of one end come back from the other, the end of
  the writing end is the end of the file, a pipe without a reading end is
  `EPIPE`, a write of `PIPE_BUF` waits for the whole block and lands whole,
  a reader slower than its writer waits instead of failing, and
  `lseek`/`readdir` of a pipe are `ESPIPE`/`ENOTDIR`.
* **The memory.** A file of the volume read through a window of mappings has
  the bytes of the file: the sum through `mmap` is the sum through `read`, a
  window slides over a file larger than the window of the machine, a private
  page takes a write the file does not, a shared page is the file after
  `msync` and another program reads the bytes, an anonymous page reads as
  zeros, the heap grows by pages and a heap past the stack is `ENOMEM`
  (`ports/port_x86_64/programs/mmap.c`, `make x86-smoke`).
* **The image is not a copy.** A program of 4 MiB runs with the heap of the
  kernel unchanged while it runs and after it: the image is mapped from the
  file and no block of the kernel holds it (`/bin/big`, `make x86-smoke`).
* **The wait.** `poll` answers the state of two pipes at once, the byte that
  arrives while the task waits (a task of its own writes it), the deadline
  that passes, a list of no descriptor as a sleep, the end of the writing end
  as a readable entry, a writing end whose reader is gone as the error, and a
  signal as `EINTR`. The wait costs no processor time: the task is out of the
  run queue (`tests/cases/test_ksys.c`, `test_poll`).
* **The sharing.** `dup2` makes a second descriptor of one open file: the
  write through it reaches the object the number now names, the copy
  outlives the original, and the old descriptor at the new number is closed.
  A pipe crosses a fork: the child writes, the parent reads to the end and
  sees the status of the child.
* **The edge.** A path that does not end in the window is `ENAMETOOLONG`, a
  buffer above the limit is `E2BIG`, a pointer that is not memory of the
  caller is `EFAULT`, and a wait without a child is `ECHILD`. The last one is
  covered on the machine: `/bin/badptr` hands `write`, `stat` and `read` an
  address outside the window, a range that crosses its end and the guard page
  of its stack, reads `14` from each and writes a line with a valid pointer
  afterwards (`make x86-smoke`).
* **The program.** A machine without a window of programs answers `ENOSYS` for
  an image of the format of the system, and every refusal before that answer -
  `ENOENT`, `EISDIR`, `EPERM`, `EINVAL` - is the answer of its own reason. A
  failed `execve` spends no descriptor of the caller.
* **The environment.** A vector of the system is a list of `NAME=VALUE` words
  with `PATH=/bin` first; an environment of nine words, or one whose string
  does not end, is `E2BIG`; a NULL environment and an empty one are the answer
  of the machine and not a refusal. The arguments and the environment are
  bounded apart, so a long line does not shorten the environment of the same
  call.
* **The limits.** A fresh task starts at the ceiling of the three resources,
  a lower value is accepted, a raise and an unknown resource are refused, and
  the numbers come back from a child task of their own so that a limit lowered
  by the test does not follow the next one. `open`, `pipe` and `dup2` answer
  `EMFILE` at the descriptor limit; a block over the memory limit is 0 from
  `malloc` and `ENOMEM` from the library, and `free` returns the room; a task
  over its CPU limit is marked with `SIGTERM` and ends with 143.

The corpus lives in [tests/corpus/vectors.txt](../tests/corpus/vectors.txt): 512
numbers with the boundary values mixed in (0, 1, 255, 65535, 2147483647 and so
on).
