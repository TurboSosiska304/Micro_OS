# Micro OS — the C library

The library of the system. Every function of it is a call of the ABI of the
system calls, so a program of the system reaches the kernel the same way a
program of Linux does.

The library is portable C11 and it knows nothing about the kernel: it knows
the contract of the calls (see [syscall-abi.md](syscall-abi.md)).

## The library of the kernel and the library of a program

This document is about the library **of the kernel** (`components/libc/src`):
it is linked into the kernel and calls `ksys_*`, so the shell and the
components use it with no crossing of a privilege boundary.

A program cannot call those functions: it was not compiled into the kernel and
their addresses are not its own. Its library is `components/libc/user` (ADR
0016): `_start`, the door of the call, `open`, `close`, `read`, `write`,
`lseek`, `readdir`, `stat`, `mkdir`, `unlink`, `getcwd`, `getpid`, `uptime`,
`execve`, `exit`, `printf`, `puts`, `snprintf`, the strings, the allocation
and the streams. The door is `int $0x80` on x86-64 and the address of a
function of the port on the board (ADR 0018); the numbers and the arguments
of the calls are the same on both machines. The two libraries share the
header of the ABI and nothing else, and that is deliberate: one runs with the
kernel under it, the other has nothing but the gate.

The library of a program grew with its utilities (0.7): `stat` to tell a file
from a directory (`rm` refuses a directory without `-r`), `mkdir` and
`unlink` for the tree (`mkdir`, `rm`), `getcwd` for the directory of the task
(`pwd`), and `strstr` for a word in a line (`grep`). The first port of a
program of somebody else (zlib, `/bin/zcat`) added the three families a
portable source expects, and they are still small enough for the board
because a program selects the files it links:

| Part | Files | What it is |
|---|---|---|
| the allocation | `src/malloc.c` | an arena in the bss of the program (`-DULIBC_HEAP_SIZE`, 256 KiB by default, 512 KiB for `zcat`), not `KSYS_MALLOC`: the block of the kernel is memory of ring 0 and a program cannot read it |
| the error | `src/errno.c` | `errno` as a global of the program and `strerror`; the calls keep the negative number of the ABI and the wrappers also store its magnitude, so a portable library reads `errno` after `read` |
| the streams | `src/file.c` | `fopen`, `fclose`, `fread`, `fwrite`, `fgetc`, `fputc`, `fgets`, `fputs`, `feof`, `ferror`, `fseek`, `ftell`, `rewind` over a descriptor; the binary mode is accepted and ignored because no file of the system translates a byte |

The names a portable source includes (`<string.h>`, `<stdlib.h>`, `<stdio.h>`,
`<errno.h>`, `<unistd.h>`, `<fcntl.h>`, `<sys/types.h>`) are thin headers in
`components/libc/user/include`, each one including `ulibc.h`. The body of the
library is still `ulibc.h`, and a program that wants the functions of the
system includes `ulibc.h` and not the headers of the host.

The formatter is the one of a program: `%d %i %u %o %x %X %c %s %p %%`, the
flags `-` and `0`, the width, and the length `l`/`z`. There is no precision
and no floating point, and the programs are built with `-mgeneral-regs-only`.

The list is what the programs of `/bin` and the first port use and not a
standard library: a function without a caller of this tree is not in it.
`cat`, `wc`, `head` and `grep` read descriptor 0 when they are given no file,
which is the shape of a filter; a fresh table of a program has no descriptor
0 yet, so that read is `EBADF` until a redirection opens it
([`docs/debts.md`](debts.md)). The recipe of the next port is
[`porting-programs.md`](porting-programs.md).

## The names

The functions keep their own names in the binary (`microos_printf`,
`microos_open`) and the usual names are macros:

```c
#include "libc/stdio.h"
#include "libc/unistd.h"

printf("hello, %s\n", name);   /* microos_printf */
```

The reason is the library of the compiler: the board already has one, and
two `printf` in one program is a conflict that the linker resolves in the
favour of one of them, in a way that depends on the order of the files.
The macros keep the programs readable and the binary unambiguous.

## The formatting

| Format | Meaning |
|---|---|
| `%d` `%i` | the signed number |
| `%u` | the unsigned number |
| `%x` `%X` | the number in the base 16 |
| `%o` | the number in the base 8 |
| `%c` | the character |
| `%s` | the string |
| `%p` | the pointer |
| `%f` `%F` | the number with a point |
| `%e` `%E` | the number with an exponent |
| `%g` `%G` | the number, the shorter of the two forms |
| `%%` | the percent |

The flags: `-` (to the left), `+` (the sign of the positive numbers), ` `
(a space before the positive numbers), `0` (the zeros in front), `#` (the
prefix of the base). The width and the precision are written as usual, `*`
takes them from the arguments. The length modifiers `h`, `l`, `ll`, `z`, `j`
and `t` are understood.

The functions:

```c
int microos_snprintf(char *buf, size_t size, const char *fmt, ...);
int microos_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int microos_printf(const char *fmt, ...);
int microos_vprintf(const char *fmt, va_list ap);
int microos_fprintf(int fd, const char *fmt, ...);
int microos_puts(const char *text);
int microos_putchar(int ch);
```

`snprintf` returns the number of the characters that were written, even when
the buffer was too short (the same as the usual library). `printf` and
`fprintf` write through the ABI, to the descriptor 1 or 2 - the console of
the system - and to the display of the board.

## The files

```c
int microos_open(const char *path, int flags, ...);
int microos_close(int fd);
long microos_read(int fd, void *buf, size_t count);
long microos_write(int fd, const void *buf, size_t count);
long microos_lseek(int fd, long offset, int whence);
int microos_unlink(const char *path);
int microos_mkdir(const char *path);
int microos_chmod(const char *path, unsigned mode);
int microos_chown(const char *path, unsigned owner);
int microos_execve(const char *path, char *const argv[]);
```

The flags of the open are the numbers of POSIX: `MICROOS_O_RDONLY`,
`MICROOS_O_WRONLY`, `MICROOS_O_RDWR`, `MICROOS_O_CREAT`, `MICROOS_O_TRUNC`,
`MICROOS_O_APPEND`.

## The memory

```c
void *microos_malloc(size_t size);
void *microos_realloc(void *ptr, size_t size);
void *microos_calloc(size_t count, size_t size);
void microos_free(void *ptr);
```

The memory comes from the memory manager of the kernel through the ABI of
the system calls. The priority of the task decides where it comes from:
the system utilities get the internal RAM, the applications get the PSRAM.

`calloc` gives the memory empty, and `realloc` keeps the data of the old
block. `realloc` of a block to the size 0 frees it.

## The processes

```c
int microos_getpid(void);
int microos_getppid(void);
int microos_waitpid(int pid, int *status, int flags);
unsigned microos_sleep(unsigned ms);
int microos_yield(void);
void microos_exit(int code);
```

The flag of the waitpid is `MICROOS_WNOHANG`: the call does not wait and
answers 0 when the child is not ready.

## The limits

```c
struct microos_rlimit { uint64_t memory; uint32_t cpu_ms; uint32_t fds; };
int microos_getlimit(int which, uint64_t *value);
int microos_setlimit(int which, uint64_t value);
int microos_getrlimits(struct microos_rlimit *out);
```

The selectors are `MICROOS_RLIMIT_MEMORY`, `MICROOS_RLIMIT_CPU` and
`MICROOS_RLIMIT_FDS`. A task starts at the ceiling of the system and may only
lower a limit: a raise is -1 with `EPERM` in `microos_errno()`. The memory is
the bytes of the user blocks the task holds, the CPU time the milliseconds it
spent running, and the descriptors the open files it may take. When the CPU
limit is reached the task is marked with `SIGTERM`; when a descriptor would
pass the limit, `open`, `pipe` and `dup2` answer `EMFILE`, which
`microos_strerror()` calls "too many open files". The shell command `ulimit`
prints the three numbers and lowers them.

## The errors

The codes are the numbers of POSIX. A call that fails returns -1 and the
code is kept for `microos_errno()`; `microos_strerror()` gives the text,
including `EMFILE` (24, "too many open files") and `ENOMEM` (12, "out of
memory", the answer of a block over the memory limit of the task).

```c
int microos_errno(void);
const char *microos_strerror(int code);
```

## What the library cannot do

* **The memory of a program is not protected.** There is no `mmap` and no
  `brk`: the memory of the tasks is shared, the same way as the tasks
  themselves share it after a `fork`.
* **The standard descriptors are the console.** The descriptors 1 and 2 are
  the console of the system when they are not open as a file. `dup2` and
  `pipe` are there (`microos_dup2`, `microos_pipe`), so a program sends its
  output to a file or to another program; the shell does not parse a
  pipeline yet.
* **The console is not readable through the descriptor 0.** A program that
  wants the input of the user opens `/dev/tty0`. The utilities of
  `components/libc/user` do read descriptor 0 for a line, but the shell does
  not carry a file or the console into a program, so the call answers
  `EBADF` and not a line.
* **There is no `printf` of a floating point with the exact digits of the
  system.** The numbers agree to the precision that was asked for, but the
  rounding of the last digit can differ.

## The tests

The tests of the library live in `tests/cases/test_libc.c`. The library is
checked against the library of the system with the real numbers of the
corpus of the project: the same numbers, the same text, the same errors.
