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
that machine offers one door (ADR 0006):

| What | Where |
|---|---|
| the instruction | `int $0x80` |
| the number of the call | `rax` |
| the six arguments | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| the result | `rax` (negative is an error) |

The vector 128 of the table of the interrupts is a gate of the port, not of
the kernel: the port that can take a call puts its stub there and the port
that cannot leaves the entry empty. The gate opens for ring 3, because the
x86-64 port runs a program as a user with page tables of its own (ADR 0007).
The processor takes the `RSP0` of the TSS on the way in, so the handler runs
on a kernel stack of the program and not on the stack of the program, which
ring 3 could have pointed anywhere. The handler saves every register the
caller may lose across a C call, puts the kernel's thread pointer back,
turns the interrupts on (they are off for the first instructions, and on
again for the call itself, which is what lets `sleep` and `yield` really
wait), calls `ksys_call()` and returns with `iretq`, setting the thread
pointer to zero first because the kernel's TLS block is not mapped in ring 3.

The registers are the ones of Linux: `rcx` and `r11` are not arguments
because the instruction `syscall` of a 64-bit machine keeps them for the
return address and the flags, and one convention for both instructions costs
nothing.

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
| 36 | `getuid` | - | the uid of the task |
| 37 | `geteuid` | - | the uid of the task (one credential) |
| 38 | `getgid` | - | the gid of the task |
| 39 | `setuid` | `uid` | 0, or an error |
| 40 | `setgid` | `gid` | 0, or an error |
| 41 | `rmdir` | `path` | 0, or an error |
| 42 | `fstat` | `fd, buf` | 0, or an error |

The numbers 0 to 22 are frozen, 23 to 26 were the next free ones; pipes took
27 and 28 (ADR 0014), signals the three after them (ADR 0015), the limits of
a task 32 and 33, the atomic rename 34 (ADR 0024 §5), the cut of an open
file 35 and the credentials of a task 36 to 40 (the second half of 1.4), and
`rmdir` and `fstat` 41 and 42 (the filesystem of 1.5). A
call that is not in the table is `ENOSYS`, never a guess at what it might
have meant.

## The control commands of `ioctl`

The call has three arguments (`fd, cmd, arg`) and the commands are the
contract of the object behind the descriptor. The numbers are frozen like the
numbers of a system call: a new command takes the next free one.

| Command | Where | Meaning |
|---|---|---|
| `FBIO_TAKE` 0x4D01 | `/dev/fb0` | the console stops drawing; the picture is the program's |
| `FBIO_GIVE` 0x4D02 | `/dev/fb0` | the console repaints its grid and draws again; a give while another program holds the device changes nothing, so a program that starts a program keeps the picture |

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
`0x03000000` are all "not there". The kernel then copies the bytes with
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

## The mode and the owner of an object

Every object has a mode and an owner. The mode is nine bits of `rwx` for the
user, the group and the others, and the owner is a uid; a new file is `0644`
and a new directory `0755`. `stat` reports both, in the `struct vfs_stat` it
copies to the caller, and `chmod` (number 23) and `chown` (number 24) change
them one at a time.

Every task has a **uid** and a **gid** (numbers 36 to 40 read and change
them). A task takes both from the task that created it; a fork, a program and
an `execve` keep them, so `getuid()` answers the identity the caller was
started with and not a constant. The root of the system is uid 0 (`KSYS_UID`):
the boot, the kernel itself and the shell of the console, until a person types
`login` or `su` (ADR 0033). The accounts are `/etc/passwd` (a name, a uid, a
gid, a home and a shell) and `/etc/shadow` (a salted SHA-256 of a password),
read by the shell and by `getpwnam`/`getpwuid` of the library - the same file
for the two, so a program and the console cannot disagree about a name.

* a task whose uid is the owner of an object reads the triad of the owner;
  every other task reads the triad of the others, because no filesystem of
  this system stores a group - the gid is kept and inherited, and the triad of
  the group is never the answer (`docs/debts.md`);
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
alone, and `mount`/`umount` to root alone; a task of another uid learns
`EPERM`. `setuid` and `setgid` follow the same shape: root names any uid of
the fifteen bits of a record, and a task of another uid names only the number
it already has. There is one credential and not the three of POSIX, so
`geteuid` answers what `getuid` answers and a task that gives up root cannot
name zero again with `setuid`. The door to another identity is the console:
`login` and `su` are commands of the shell of the kernel, they check
`/etc/shadow` and move the two numbers of the *shell* task together
(`task_set_creds`, the gid first), and every program the person starts
inherits them. There is no setuid bit in the image of a program, so a program
cannot change the identity it runs with (ADR 0033).

The mode of `chmod` is refused outside the nine bits (`EINVAL`) and so is an
owner outside the fifteen bits of a uid (`EINVAL`): a number that does not fit
would spill into the type of the object. `chmod 000` is not expressible
either: a zero in the record means "the record predates the modes", so the
filesystem refuses it instead of storing a number it would read back as
`0644`. `stat` checks nothing and reports the two numbers to whoever asks.

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

`stat` has no field for a time, and its structure and its number are frozen.
`statx` (number 26) is the second door: the first five fields of `struct
vfs_statx` are the five fields of `struct vfs_stat`, in the same order, and
the two last fields are the moments of the object.

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

The moments are the seconds of Unix in 64 bits, so the year 2038 is not a
wall of the contract. They come from the clock of the kernel
(`k_clock_now_unix()`): the RTC of the board, the process of the host and the
NTP service all feed the same number. A machine whose clock was never set -
the host build, the x86-64 machine without a battery - stamps with the
seconds since its boot, which orders the changes but is not a date;
`k_clock_present()` says which one it is.

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

## The programs

`execve(path, argv, envp)` makes the caller a program:

* the file is read through the VFS, not through a descriptor of the caller,
  so a failed call leaves the table of the task exactly as it was;
* a program is at least the 32 bytes of the header of `kernel/exec.h` and at
  most `K_EXEC_MAX_IMAGE` (8 MiB, the memory of the machine); the image is
  read into one block of the kernel in pieces of `KSYS_MAX_IO` (64 KiB), so
  the transfer edge of a call is not the limit of the file. A file outside
  that range is refused before a byte of it is read: `E2BIG` past the ceiling
  and `EINVAL` below the header;
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
program (the status belongs to `waitpid` of that task, which is the child of
the caller). This is the compatibility shape of a machine whose shell is not a
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

There is no job control: no `SIGSTOP`, `SIGCONT` or `SIGTSTP`, no groups and
no `&`. Ctrl+C at the console is the one signal of the terminal: while the
shell waits for the program it started, a Ctrl+C byte sends `SIGINT` to
that program, and while it waits for a pipeline to every stage of the job
and the programs they started (ADR 0017). `kill` from the prompt sends a
signal to a pid from `ps`.

The library has the same calls as `microos_kill()` and
`microos_signal()`, with `MICROOS_SIGINT` and the other three numbers, and
the two dispositions `MICROOS_SIG_DFL` and `MICROOS_SIG_IGN`;
`microos_signal()` answers `MICROOS_SIG_ERR` and sets `microos_errno()`
when the signal is not one of the system.

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
The pipe is a ring of 256 bytes of the kernel; the system keeps eight of
them. The place for the answer is checked at the edge: NULL is `EINVAL`, a
place that is not the memory of the caller is `EFAULT`.

* A read of an empty pipe whose writing end is still open waits, and then
  answers `EAGAIN` if the wait passed its bound: a timeout is not the end of
  the file. A read of an empty pipe with no writer is 0, the end of the file.
* A write with no reading end is `EPIPE` (`KERR_PIPE`, the number 32 of
  POSIX). There is no `SIGPIPE` in this system yet, so the error is the whole
  answer; the bytes written before the reader went away are the count.
* The access of each end is the flag of its descriptor: `read` of the writing
  end and `write` of the reading end are `EBADF`. A pipe has no position, so
  `lseek` is `ESPIPE` (`KERR_SPIPE`, 29), and it has no entries, so `readdir`
  is `ENOTDIR`.
* A wait of a pipe is a poll with a bound, not a queue of waiters: 500 ms.
  A read that found no byte in that time is `EAGAIN`; a write that placed no
  byte is `EAGAIN` too, and one that placed some bytes answers the count it
  placed. A queue with a wakeup is the next step.

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
| `KSYS_RLIMIT_MEMORY` (1) | the user blocks the task holds | bytes | 2 MiB (the PSRAM of the board) |
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
  `EPIPE`, the wait of an empty or a full pipe passes its bound as `EAGAIN`,
  and `lseek`/`readdir` of a pipe are `ESPIPE`/`ENOTDIR`.
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
