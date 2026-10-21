# Micro OS — the C library

The library of the system. Every function of it is a call of the ABI of the
system calls, so a program of the board reaches the kernel the same way a
program of Linux does.

The library is portable C11 and it knows nothing about the kernel: it knows
the contract of the calls (see [syscall-abi.md](syscall-abi.md)).

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
```

The flags of the open are the numbers of POSIX: `MICROOS_O_RDONLY`,
`MICROOS_O_WRONLY`, `MICROOS_O_RDWR`, `MICROOS_O_CREAT`, `MICROOS_O_TRUNC`,
`MICROOS_O_APPEND`.

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

## The errors

The codes are the numbers of POSIX. A call that fails returns -1 and the
code is kept for `microos_errno()`; `microos_strerror()` gives the text.

```c
int microos_errno(void);
const char *microos_strerror(int code);
```

## What the library cannot do

* **The memory of a program is not protected.** There is no `mmap` and no
  `brk`: the memory of the tasks is shared, the same way as the tasks
  themselves share it after a `fork`.
* **The standard descriptors are the console.** The descriptors 1 and 2 are
  the console of the system when they are not open as a file. There is no
  `dup2` yet, so a program cannot send its output to a file.
* **The console is not readable through the descriptor 0.** A program that
  wants the input of the user opens `/dev/tty0`.
* **There is no `printf` of a floating point with the exact digits of the
  system.** The numbers agree to the precision that was asked for, but the
  rounding of the last digit can differ.

## The tests

The tests of the library live in `tests/cases/test_libc.c`. The library is
checked against the library of the system with the real numbers of the
corpus of the project: the same numbers, the same text, the same errors.
