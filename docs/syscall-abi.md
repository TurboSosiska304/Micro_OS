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
| 25 | `execve` | `path, argv` | never returns; 0 when the program returns; or an error |

The numbers 0 to 22 are frozen, and 23 to 25 were the next three free ones.
A call that is not in the table is `ENOSYS`, never a guess at what it might
have meant.

The flags of `open` are the same as the ones of the VFS:

| Flag | Meaning |
|---|---|
| `KSYS_O_RDONLY` | only to read |
| `KSYS_O_WRONLY` | only to write |
| `KSYS_O_RDWR` | to read and to write |
| `KSYS_O_CREAT` | create the file if it is not there |
| `KSYS_O_TRUNC` | cut the file to zero |
| `KSYS_O_APPEND` | write at the end |

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
in the window is `E2BIG`, because the contract has no `ENAMETOOLONG`.

**A buffer** (`buffer_check`) is NULL only with a zero length, and no call
touches more than `KSYS_MAX_IO` (65536 bytes) in one exchange. The number is
far above any buffer of the system and well below what could not be a real
transfer on a machine with ~100 KB of RAM. NULL with a length is `EINVAL`, a
length above the limit is `E2BIG`. A length inside the limit is still not a
memory range: without an MMU the kernel cannot know where a pointer of the
caller points, and this document does not pretend otherwise.

**The flags of an `open`** (`open_flags_check`) are one of the three access
modes - both bits at once is not a mode - and no bit outside
`KSYS_O_ALLOWED`. The flags are checked before the file is looked for, so the
answer is about the argument and not about the object.

**The `whence` of `lseek`** and **the pid of `waitpid`** are compared before
a cast to `int` can cut a wide register down to a valid value: `2^32 + 1` is
not `SEEK_CUR`, and `2^32 + 5` is not the child 5. The `whence` is one of
three numbers; a pid below -1 names no task, and only 0 and -1 mean "any
child"; the flags of `waitpid` are one bit. All of them are `EINVAL`.

**`stat` and `getcwd`** need the place for the answer: NULL there is a
mistake of the caller, not "the answer is not wanted", and it is `EINVAL`
instead of a question to a filesystem that has nowhere to put its answer.

**`readdir`** takes whole entries: a count that is not a multiple of
`struct vfs_dirent` would let the last entry land past the memory of the
caller, so it is `EINVAL`.

## The mode and the owner of an object

Every object has a mode and an owner. The mode is nine bits of `rwx` for the
user, the group and the others, and the owner is a uid; a new file is `0644`
and a new directory `0755`. `stat` reports both, in the `struct vfs_stat` it
copies to the caller, and `chmod` (number 23) and `chown` (number 24) change
them one at a time.

The system has **one user**, uid 0 (`KSYS_UID`), and the caller of a system
call is always that user:

* the object is compared with that uid: an object owned by uid 0 is answered
  by the triad of the owner, and an object owned by any other number by the
  triad of the others, because the caller is not its owner and there is no
  group in this system;
* **there is no bypass for uid 0.** The exemption of root is the one part of
  the POSIX rule that is left out: a file of the mode `0200` cannot be read by
  the only user of the system either. The owner is a number that is written
  and checked, not a privilege (ADR 0005).

`open` compares the access in its flags with the mode of the object before a
descriptor exists, so every filesystem under the VFS answers a forbidden open
the same way - `EPERM`. A file that is not there is not checked: with
`O_CREAT` it is about to be made with the mode of the design, and without
`O_CREAT` the open itself answers `ENOENT`. A zeroed mode is not "nobody may
touch it": it means "this filesystem keeps no modes" - `procfs` and `devfs` -
and those objects keep the access they always had.

The mode of `chmod` is refused outside the nine bits (`EINVAL`) and so is an
owner outside the fifteen bits of a uid (`EINVAL`): a number that does not fit
would spill into the type of the object. `chmod 000` is not expressible
either: a zero in the record means "the record predates the modes", so the
filesystem refuses it instead of storing a number it would read back as
`0644`. `stat` checks nothing and reports the two numbers to whoever asks;
the mode of a directory does not decide the search of its children yet.

## The programs

`execve(path, argv)` makes the caller a program:

* the file is read through the VFS, not through a descriptor of the caller,
  so a failed call leaves the table of the task exactly as it was;
* a program is at least the 32 bytes of the header of `kernel/exec.h` and at
  most `KSYS_MAX_IO` bytes, because the image is read into one block of the
  kernel before the port copies it; a file outside that range is refused
  before a byte of it is read;
* the image must be a `MOX1` of this version, and the port is then asked
  whether the machine can run a program at all. The board and the simulator
  answer no, and the answer of the call is `ENOSYS`; the x86-64 port names a
  window of memory, copies the image there and enters it.

The errors: `ENOENT` for a path that names nothing, `EISDIR` for a directory,
`EPERM` for a file whose mode has no execute bit for the caller (`0400` is not
a program), `EINVAL` for a file that is not an image of this system, `ENOMEM`
for an image the port could not place in its window, and `ENOSYS` for a
machine without a window of programs.

A successful call **does not return** when the program never comes back: the
program is the caller from then on. A program that returns ends the call
with 0 - the state of a machine with no process to exit into, where the
kernel has to notice that the program has finished. The vector of the
arguments is the memory of the program and the kernel does not walk it -
without an MMU there is nothing to check an array of pointers against - and
today the port is entered without a vector, because `k_exec_run()` has no
place for it: a program of this system reads no arguments yet.

## The processes

The kernel has no memory management, so a `fork` cannot copy the memory of
the parent: the tasks **share** it, like the flag `CLONE_VM` of Linux. The
child gets its own number, and the program tells the child by the mark of its
task (`task_self()->is_child`) - in Linux the child knows it by the zero that
`fork()` returns, here the child starts the program from the beginning.

`waitpid()` waits for a child, the status is the code the child finished
with. The number 0 or -1 means "any child".

## The descriptors

Every task has its own table of the open files, four of them. The descriptor
is the number of the slot. A descriptor that was never opened gives
`KERR_BADF`.

A descriptor remembers the access it was opened with - read, write, or both.
The kernel checks that access at the edge of the system, so every filesystem
under it answers the same way: reading a descriptor that was opened to be
written to, or writing one that was opened to be read from, gives
`KERR_BADF`. The mode of the object is a second, earlier check (see "The mode
and the owner of an object"): an open that the mode does not allow never
becomes a descriptor at all, and the answer is `EPERM`.

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
names of POSIX: `microos_chmod`, `microos_chown` and `microos_execve` (and the
usual `chmod`, `chown`, `execve` when its header is included). They return -1
and set `microos_errno()` when something goes wrong, and they refuse the two
numbers of a mode and of an owner that a record cannot hold before the kernel
is asked.

## The tests

The tests of the ABI do not check that "something" works, they check the
**properties**: the same data has to give the same result no matter how the
program breaks it into pieces.

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
  decides for uid 0 and the triad of the others for any other owner, there is
  no bypass for the only user of the system, and a mode that the record cannot
  hold is refused instead of stored.
* **The program.** A machine without a window of programs answers `ENOSYS` for
  an image of the format of the system, and every refusal before that answer -
  `ENOENT`, `EISDIR`, `EPERM`, `EINVAL` - is the answer of its own reason. A
  failed `execve` spends no descriptor of the caller.

The corpus lives in [tests/corpus/vectors.txt](../tests/corpus/vectors.txt): 512
numbers with the boundary values mixed in (0, 1, 255, 65535, 2147483647 and so
on).
