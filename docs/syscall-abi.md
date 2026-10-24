# The ABI of the system calls

> Russian version: [syscall-abi_ru.md](syscall-abi_ru.md)

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
`KERR_BADF`. There are no owners and no mode bits on the files - the system
has a single user, and the access of the descriptor is the whole of the
permission model. Owners and mode bits are planned for after the first
release.

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
  that a change of the contract is noticed at once.
* **The errors.** A file that does not exist, a descriptor that was never
  opened: the same error, no matter how the call was made.
* **The agreement.** The file that the system calls write is the file that the
  filesystem sees.

The corpus lives in [tests/corpus/vectors.txt](../tests/corpus/vectors.txt): 512
numbers with the boundary values mixed in (0, 1, 255, 65535, 2147483647 and so
on).
