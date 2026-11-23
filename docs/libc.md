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

The library of a program grew with its utilities (0.7): the record of a path
to tell a file from a directory (`rm` refuses a directory without `-r`;
the POSIX `stat` of the third port took that place), `mkdir` and
`unlink` for the tree (`mkdir`, `rm`), `getcwd` for the directory of the task
(`pwd`), and `strstr` for a word in a line (`grep`). The port of zlib
(`/bin/zcat`) added the three families a portable source expects, the port of
Lua the arithmetic of a double, and the ports of sbase and of file the
families of a Unix-like system (`getopt`, `fnmatch`, `regex`, `pwd`,
`termios`, the reader of a line). They are still small enough for the board
because a program selects the files it links: the list of a program is in
`ports/port_x86_64/CMakeLists.txt`, and a command of sbase does not carry the
formatter of a double nor the database of libmagic.

| Part | Files | What it is |
|---|---|---|
| the allocation | `src/malloc.c` | an arena in the bss of the program (`-DULIBC_HEAP_SIZE`, 256 KiB by default, 512 KiB for `zcat` and 960 KiB for `file` and `zstd`), not `KSYS_MALLOC`: the block of the kernel is memory of ring 0 and a program cannot read it; `calloc` and `realloc` are the same arena |
| the error | `src/errno.c` | `errno` as a global of the program and `strerror`; the calls keep the negative number of the ABI and the wrappers also store its magnitude, so a portable library reads `errno` after `read` |
| the streams | `src/file.c` | `fopen`, `fdopen`, `fmemopen`, `freopen`, `fclose`, `fread`, `fwrite`, `fgetc`, `getc`, `getchar`, `ungetc`, `fputc`, `fgets`, `fputs`, `perror`, `feof`, `ferror`, `fseek`, `ftell`, `rewind`, `fileno`, `fprintf` over a descriptor; a block of 512 bytes in the object holds the read-ahead, so a reader of a few hundred kilobytes is not one call per byte; the binary mode is accepted and ignored because no file of the system translates a byte; `tmpfile` is a failure (no `/tmp`) |
| the formatter | `src/stdio.c`, `src/format_float.c` | `printf`, `puts`, `putchar`, `snprintf`/`vsnprintf`/`sprintf`, and `fprintf`/`vfprintf` over the streams; the conversions are the ones of the table below, and a build that defines `ULIBC_WITH_FLOAT` adds the digits of a double from `src/format_float.c` |
| the reader of a line | `src/format_ext.c` | `getline`/`getdelim`: the buffer of the caller grows with the line, which is what a command of a Unix-like system is written on |
| the conversions and the sort | `src/stdlib.c` | `strtod`, `strtol`/`strtoul`/`strtoll`/`strtoull`, `atoi`/`atol`/`atof`, `abs`/`labs`, `qsort` (a merge sort in the arena, insertion when it has no room), `getenv` over `environ`. A build without `ULIBC_WITH_FLOAT` links the file and not `strtod`/`atof`, which are in the float build alone |
| the numbers and the order of sbase | `src/stdlib_ext.c` | the same conversions with the `ERANGE` of an overflow, `atoll`/`llabs`, the heap sort of `qsort` and the halving walk of `bsearch`, and `getenv`; a command of sbase links this file and a program of another port the one above, so no program carries two answers to one name |
| the information about an object | `src/stat.c`, `include/sys/stat.h` | the POSIX `struct stat` with the `S_IS*` macros, `stat`/`lstat` (one call: the system has no links) over `KSYS_STATX`, `fstatat`, `fstat` over `KSYS_FSTAT` (the mode, the owner, the size and the two moments of the open object), `chmod`, `umask`. The record of the ABI keeps its own name, `ulibc_stat_call`, and fills `struct vfs_stat` |
| the entries of a directory | `src/dirent.c`, `include/dirent.h` | `DIR`, `struct dirent`, `opendir`, `fdopendir`, `readdir`, `closedir`, `rewinddir`, `dirfd` over a batch of eight names of `ulibc_readdir` |
| the path and the directory | `src/path.c`, `src/syscall.c` | the resolver of a relative path against the directory of the program (the ABI has no call that moves it), `.`/`..` folded, and the table of the directories an open named (`fdopendir`) |
| the halves of a path, the copy of a word | `src/libgen.c`, `src/string_ext.c` | `basename`/`dirname`, `strdup`/`strndup` (they allocate, and a program without an arena does not link them), `strspn`/`strcspn`/`strpbrk` (in `src/string.c`, which every program links), the compare without the case of a letter |
| the patterns and the options | `src/fnmatch.c`, `src/regex.c`, `src/getopt.c` | the pattern of a name, the regular expressions of `grep` and `sed`, and the options of a command |
| the users and the terminal | `src/pwd.c`, `src/termios.c` | the one owner of the system (`getpwnam` and its neighbours) and the shape of a terminal the release of a library sets |
| the arithmetic | `src/math.c` | the functions of `<math.h>` for a `double`: the square root, the logarithm and the exponential of the x87 unit, the trigonometric functions of the unit with an argument reduced by `fprem`, and the rounding and the split of the exponent in integers |
| the long jump | `src/setjmp.c` | `setjmp`/`longjmp` in the six callee-saved registers, the stack pointer and the return address of x86-64 |
| the clock and the locale | `src/time.c`, `src/locale.c`, `src/ctype.c`, `src/assert.c` | `time` (the uptime: the ABI has no wall clock), `clock`, `clock_gettime`, `gmtime`/`localtime`/`mktime`/`strftime` of the C locale, `asctime`/`ctime` (the 26 bytes of a moment), `localeconv`, the classes of a byte, `assert` |
| the exit | `src/atexit.c` | the handlers of `atexit`, run by `exit` in the reverse order of their registration |
| the ends of a POSIX library | `src/unistd_ext.c`, `src/asprintf.c` | `access`, `chdir`, `readlink`, `pipe`, `dup2`, `fork`, `_exit`, `wait`, `sleep`, `sysconf`, `isatty` and the `asprintf`/`vasprintf` of the arena - the calls a library of a Unix-like system expects of a machine that has a task and a descriptor |

The names a portable source includes (`<string.h>`, `<stdlib.h>`, `<stdio.h>`,
`<errno.h>`, `<unistd.h>`, `<fcntl.h>`, `<sys/types.h>`, `<sys/stat.h>`,
`<dirent.h>`, `<libgen.h>`) are thin headers in
`components/libc/user/include`, each one including `ulibc.h`. The port of
file added the headers a library of a Unix-like system asks for beside them:
`<stdint.h>` and `<inttypes.h>` (the types of a width in bits and the `PRIx64`
of the formatter), `<sys/param.h>` (`MIN`, `MAX`, `howmany`) and `<locale.h>`
(the C locale, `setlocale` and `localeconv`); the functions it needed are
`asprintf`/`vasprintf` (a formatted string of the arena), `asctime`/`ctime`
(the 26 bytes of a moment), `pread` (the read at an offset of a descriptor,
in `third_party/file/src/pread.c`) and the `PIPE_BUF` of `<limits.h>`. The body of the
library is still `ulibc.h`, and a program that wants the functions of the
system includes `ulibc.h` and not the headers of the host.

The formatter is the one of a program: `%d %i %u %o %x %X %c %s %p %%`, the
flags `- + space # 0`, the width and the precision (a `*` takes either from
the arguments), and the length `l`/`ll`/`z`/`j`/`h`. A build that defines
`ULIBC_WITH_FLOAT` adds `%f %F %e %E %g %G`: the digits come from
`src/format_float.c`, and the include of that source is the choice of a
program with a `double` in it (Lua). A program of the tree that does not
define the name is built with `-mgeneral-regs-only`, where a double has no
register, and prints such a conversion as the characters it was written with.
`%a` is not implemented.

Floating point in the guest is the one place the port leaves its rule:
`CR4.OSFXSR`, `CR4.OSXMMEXCPT` and `CR0.NE` are set by the loader, the
program is built without `-mgeneral-regs-only`, and the scheduler saves
neither the XMM registers nor the x87 state. A program with a point in its
numbers therefore runs alone - `execve` waits for it - and a second one at the
same time needs the save of the state (`docs/debts.md`).

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

## The library of a program: the POSIX families

`components/libc/user` is the library a program is linked against (ADR 0016).
It is not the library of the kernel above: it reaches the kernel only through
`int $0x80` on x86-64 (or the door of the board), and it grew with the
programs that were ported to it. The port of sbase (ten Unix tools) needed
the families a portable POSIX source expects, and they are now part of it:

| Header | The functions | The file |
|---|---|---|
| `<dirent.h>` | `opendir`, `readdir`, `closedir`, `rewinddir`, `fdopendir` over `KSYS_READDIR`, one batch of eight entries per directory | `src/dirent.c` |
| `<sys/stat.h>` | `stat`, `lstat`, `fstat`, `fstatat`, `chmod`, `umask` and `struct stat` with the `S_IS*` macros over `KSYS_STATX`; `fstat` asks `KSYS_FSTAT` for the mode, the owner and the moments of the open object | `src/stat.c` |
| `<unistd.h>` | `access`, `chdir`, `rmdir`, `readlink`, `isatty`, `pipe`, `dup2`, `fork`, `wait`, `waitpid`, `execvp`, `_exit`, `sleep`, `sysconf`, `getopt`; `rmdir` is `KSYS_RMDIR`, not `unlink` of a directory | `src/unistd_ext.c`, `src/getopt.c` |
| `<pwd.h>`, `<grp.h>` | the credentials of the task (`getuid`, `geteuid`, `getgid`, `setuid`, `setgid`); only uid and gid 0 have a name, `root` | `src/pwd.c` |
| `<time.h>` | `time`, `clock`, `gmtime`/`localtime`, `strftime`, `mktime`, `clock_gettime` | `src/time.c` |
| `<regex.h>` | a POSIX basic/extended engine: literals, `.`, `*`, `+`, `?`, `{m,n}`, groups, back references, alternation, brackets and the classes | `src/regex.c` |
| `<fnmatch.h>` | the pattern of a name with `*`, `?`, `[...]`, `FNM_PATHNAME` and `FNM_NOESCAPE` | `src/fnmatch.c` |
| `<stdlib.h>` | the conversions with `ERANGE` (`strtod` only in a program with `ULIBC_WITH_FLOAT`), `qsort` (merge sort), `bsearch`, `atoll`/`llabs`, `getenv`, `atexit`, `abort`, `srand`/`rand` | `src/stdlib.c`, `src/rand.c` |
| `<string.h>`, `<strings.h>`, `<ctype.h>` | `strdup`, `strndup`, `strspn`, `strcspn`, `strpbrk`, `strcasecmp`, `strcasestr` and the ASCII classes | `src/strdup.c`, `src/string_ext.c`, `src/ctype.c` |
| `<stdio.h>` | `fprintf`/`vfprintf`, `sprintf`, `putchar`, `getline`/`getdelim`, `fmemopen`, `fdopen`, `fileno`, `ungetc`, `perror`, `sscanf`, and `popen`/`pclose` (answered `ENOSYS`) | `src/stdio.c`, `src/stdio_ext.c`, `src/file.c`, `src/sscanf.c`, `src/unsupported.c` |
| `<stdlib.h>` (the real numbers) | `strtod`, `atof` (the decimal form of the C locale, the value of the machine's rounding), and `setenv`/`unsetenv` (answered `ENOSYS`: the environment is the vector of `_start`) | `src/stdlib.c`, `src/unsupported.c` |
| `<math.h>` | `fabs`, `floor`, `ceil`, `trunc`, `round`, `fmod`, `modf`, `frexp`, `ldexp`, `sqrt`, `pow`, `exp`, `log`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, over the x87 unit (a program built with `FLOAT`) | `src/math.c` |
| `<signal.h>` | `signal`, `raise`, `SIGINT`, `SIGPIPE`, `SIGTERM`, `SIGCHLD`, `SIG_DFL`, `SIG_IGN` | `src/syscall.c` |
| `<sys/time.h>`, `<sys/resource.h>` | `gettimeofday` (the uptime), `utimes` (`ENOSYS`), `getrusage` (`ENOSYS`), `usleep` (a sleep of the task) | `src/gettimeofday.c`, `src/resource.c`, `src/usleep.c`, `src/unsupported.c` |
| `<termios.h>` | the two calls of a terminal, answered `ENOTTY`: the console has no control command | `src/termios.c` |
| `<stdio.h>` (the names) | `rename` (POSIX puts it here, in `src/syscall.c`), and `usleep` in `<unistd.h>` | `src/syscall.c`, `src/usleep.c` |

The stream of `<stdio.h>` is a descriptor and two flags, with no buffer of
the host; `fmemopen` is a block of the caller that a command uses for a word
of its line. The arena of `malloc` is in the bss of the program (256 KiB by
default, `ULIBC_HEAP_SIZE` for a tool that holds a file), because a block of
`KSYS_MALLOC` is memory of ring 0 that a program cannot read.

Three shapes of the machine are visible in the library and are decisions, not
bugs:

* **There is no `chdir` in the ABI.** The shell changes the cwd of its task
  (`cd`) and `execve` carries it into a program, but a program has no call
  that moves it. The library keeps the directory itself (`src/path.c`) and
  resolves every relative path against it, so `ls a b`, `find .` and the
  `chdir` of `ls` between two directories work; a child started by `execve`
  begins in the directory of the task, not in the one the program moved to.
* **There is no wall clock.** `time()` is the seconds since the boot, and
  `localtime` is UTC; the filesystem still keeps a moment of Unix in every
  record, and a listing prints it.
* **There is no floating point by default.** A program is built with
  `-mgeneral-regs-only`; a `double` cannot cross a function of the guest, so
  a program that does not ask for the `FLOAT` option has no `strtod`/`%f`, and
  `sort -n` and `ls -h` of sbase order and print their numbers as decimals in
  the port layer. The programs that do ask (SQLite and Lua) are built with
  `ULIBC_WITH_FLOAT`, `src/format_float.c`, `src/strtod.c` and `src/math.c`,
  and the loader turns the SSE and x87 units on for ring 3; the scheduler
  saves neither, so one such program runs at a time (`docs/debts.md`).

`fork` is real on x86-64 (copy on write): the child returns from the call with
0, the parent with the pid, `execve` of a program replaces the image of the
caller and `waitpid` reaps the child and hands back its status. `fork()` of
the library is the call itself - the port zeroes the answer of the child and
flushes the TLB of the pages it makes read-only, so no workaround is needed.

The status of `waitpid` is the code the program exited with (0 to 255), and a
task taken down by a signal ends with `128 + the signal`. That is not the
16-bit word of POSIX where the code and the signal sit in two fields, so
`<sys/wait.h>` reads it in those two ranges: 0 to 127 is `WIFEXITED` with
`WEXITSTATUS` the code, and 128 to 255 is `WIFSIGNALED` with `WTERMSIG` the
signal. A program that calls `exit(200)` is therefore reported as the signal
72; a code above 127 is the price of a status of one byte.

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
  output to a file or to another program, and the shell of K7 parses a
  pipeline and a redirect (ADR 0017). A descriptor that is not open is not
  the console: a file opened as 1 shadows it for the caller.
* **The console is not readable through the descriptor 0.** A program that
  wants the input of the user opens `/dev/tty0`. The utilities of
  `components/libc/user` do read descriptor 0 for a line, but the shell does
  not carry a file or the console into a program, so the call answers
  `EBADF` and not a line.
* **There is no `printf` of a floating point with the exact digits of the
  system.** The numbers agree to the precision that was asked for, but the
  rounding of the last digit can differ.

## The tests

The tests of the library of the kernel live in `tests/cases/test_libc.c`:
the library is checked against the library of the system with the real
numbers of the corpus of the project. The pure part of the library of a
program (the classes, the strings, the pattern of a name, the options, the
regular expressions, the credentials and the decimal order of sbase) is
compiled for the host by `tests/cases/test_userland.c`, which is a test of its
own (`userland`); the door the credentials of that build call is the shim of
`tests/cases/userland_door_shim.c`, and the real calls behind the door are
covered by `make x86-smoke` and `make x86-disk-smoke`.
