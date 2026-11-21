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
| 25 | `execve` | `path, argv` | never returns; 0 when the program returns; or an error |
| 26 | `statx` | `path, buf` | 0, or an error |
| 27 | `pipe` | `fds` | 0 and two descriptors, or an error |
| 28 | `dup2` | `oldfd, newfd` | `newfd`, or an error |

The numbers 0 to 22 are frozen, and 23 to 28 were the next free ones.
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
| `KSYS_O_DIRECTORY` | open a directory, for `readdir` |

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
(`k_port_exec_contains`, contract 1.6). On the board, the simulator and the
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
`0644`. `stat` checks nothing and reports the two numbers to whoever asks.

The **`x` bit of every directory of a path** is checked before the object at
the end is looked at. The VFS walks the components of the path, calls `stat`
on each directory on the way and refuses the walk with `EPERM` when the caller
may not search through it: a directory of the mode `0600` keeps its children
out of reach for `open`, `stat`, `execve` and every other call that resolves a
path, and one of `0700` lets them through. The directory that is named at the
end is not searched through - `stat` of a `0600` directory itself still works,
and opening it as a directory needs the read bit, not the search bit. A
zeroed mode is still "this filesystem keeps no modes", so the roots of
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
  window of memory, builds the page tables of the program, copies the image
  into the physical arena behind the window and enters it through an `iretq`
  into ring 3 (ADR 0007).

The errors: `ENOENT` for a path that names nothing, `EISDIR` for a directory,
`EPERM` for a file whose mode has no execute bit for the caller (`0400` is not
a program), `EINVAL` for a file that is not an image of this system, `ENOMEM`
for an image the port could not place in its window, and `ENOSYS` for a
machine without a window of programs.

A successful call **does not return** when the program never comes back: the
program is the caller from then on. A program that ends (by `exit`, or by a
fault that the port recovers) ends the call with 0 - the state of a machine
with no process to exit into, where the kernel has to notice that the program
has finished - and the status it ended with (the low byte of the status, 0 to
255) is the answer of the run, not of the call (see "The entry into the
kernel").

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
still decides the pid, the parent and the wait.

`waitpid()` waits for a child, the status is the code the child finished
with. The number 0 or -1 means "any child". A call with no child to wait for
is `ECHILD` (`KERR_CHILD`, the number 10 of POSIX), whether it was ready to
wait or not, and the same answer ends the waiting once the internal limit of
five seconds passes without a finished child. A number that names no child of
the caller is `ECHILD` too; a pid or a flag that is not of the contract is
`EINVAL` before that.

## The descriptors

Every task has its own table of the open files, four of them. The descriptor
is the number of the slot. A descriptor that was never opened gives
`KERR_BADF`.

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
before the kernel is asked.

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
  hold is refused instead of stored. A directory whose mode has no search bit
  keeps its children out of reach, and one with the bit lets them through.
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

The corpus lives in [tests/corpus/vectors.txt](../tests/corpus/vectors.txt): 512
numbers with the boundary values mixed in (0, 1, 255, 65535, 2147483647 and so
on).
