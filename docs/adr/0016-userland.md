# ADR 0016 - A userland: the entry with an environment and a library

* Status: accepted
* Date: 2026-11-21

## Context

The x86-64 port runs a program of `/bin` as a task of ring 3 with page tables
of its own (ADR 0005, ADR 0006, ADR 0007). Two things were still missing for
that to be a userland and not a demonstration:

* **the environment.** `execve(path, argv)` had no `envp`, and the entry of a
  program was `argc` and `argv` in two registers. A program could not learn
  where its files are or who it runs as, and a second vector had nowhere to
  travel: the code of the port, the seam of the kernel and the wrapper of the
  library all spoke two arguments.
* **a library.** Every program included `programs/prog.h` and wrote its own
  `strlen` and `put_dec`. That is the right way to test the raw door (ADR
  0006) and the wrong way to write `cat`: the library of the kernel is linked
  into the kernel and calls `ksys_*`, whose addresses do not exist in ring 3,
  so a program had no library at all.

The check of the release has a line for each of them: "`argv` and an
environment (`envp`) with a documented entry ABI" and "a libc subset that
builds 5-10 real programs" ([`docs/releases/1.0.md`](../releases/1.0.md)).

## Decision

### 1. The entry has three arguments

A program is entered with the arguments of a C function in the registers of
the SysV ABI of the machine:

| Register | Argument |
|---|---|
| `RDI` | `argc`, the number of the words of `argv` |
| `RSI` | `argv`, a vector of pointers terminated by NULL |
| `RDX` | `envp`, a vector of "NAME=VALUE" words terminated by NULL |

The stack is aligned as after a call, and its top word is zero: a program that
returns instead of calling `exit` faults on address zero. A C program of this
port therefore starts in `_start(long argc, char *const argv[], char *const
envp[])`, which calls `main(argc, argv, envp)` and then `exit()`.

`argv[0]` is the name of the program as the caller wrote it, as in Linux.
`envp` is never NULL: a program started without an environment sees the
vector of the system (`k_exec_environ`, four words: `PATH`, `HOME`, `USER`
and `PWD`), so every program of the machine has the same shape of the entry.
An empty environment is a vector whose first word is NULL and not a NULL
pointer - the two are different things and POSIX says so too.

### 2. `execve` gains a third argument, its number does not move

`execve` is number 25 and stays 25; the call is now
`execve(path, argv, envp)` and the third register of the convention (`RDX`)
carries the vector. A caller of the old shape passes zero there and sees the
environment of the system, which is what the shell does: `exec` in
`components/kshell` builds a line of arguments and no environment, and it did
not have to change for this.

The kernel copies both vectors at the edge of the call into the frame of the
call (`argv_copy`, twice, into a frame of its own per vector), so the port
receives strings of the kernel and the pointers of the caller never reach the
program. Each vector is at most eight words and 512 bytes; a string that does
not end is `E2BIG`, a word that is not the memory of the caller is `EFAULT`.
The environment is not a suffix of the arguments: the two are copied apart, so
the length of one never decides the room of the other.

`ksys_execve()` keeps its two arguments for the callers that have no
environment (the shell, the library of the kernel); `ksys_execve_env()` is the
same call with one. The wrapper with two arguments is the one the call had
before this decision and it now means "the environment of the system".

### 3. The image carries the environment to the port

`struct k_exec_image` gains one field, `envp`, which the parser sets to NULL
because a file has no environment. `k_exec_run()` copies the structure of the
call into its own frame, fills a NULL `envp` with `k_exec_environ()` and hands
that to the port. The port contract (1.6) does not move: the environment is
part of the image the port is asked to enter, not a seventh function of the
contract. The copy is local, so two tasks entering programs at the same time
do not share it.

The x86-64 port places the environment after the arguments in the window of
the program - the strings, then the array of pointers, with the virtual
addresses the program will see - maps the pages it needs and puts the pointer
in `RDX` before the `iretq`. The five values of `port_exec_enter_user()` are
`entry`, `argc`, `argv`, `envp` and the user stack; the two last ones ride in
`RCX` and `R8` and are kept in callee-saved registers across the call that
sets the thread pointer.

### 4. A library of a program, and not of the kernel

`components/libc/user/` is the library a program is linked against:

| File | What it is |
|---|---|
| `include/ulibc.h` | the prototypes, the flags of `open`, the descriptors |
| `src/syscall.c` | the door, `int $0x80`, and `open`, `close`, `read`, `write`, `lseek`, `readdir`, `getpid`, `uptime`, `execve`, `exit` |
| `src/start.c` | `_start`: `main(argc, argv, envp)`, then `exit` |
| `src/stdio.c` | `printf` and `puts` over a 128-byte buffer and `write(1, ...)` |
| `src/string.c` | `strlen`, `strcmp`, `strncmp`, `memcpy`, `memset` |

The header includes `kernel/ksys.h` - the same header the kernel is compiled
against - so a number of a call or a field of a structure cannot drift between
the two sides of the door. The functions return the result of the call: a
negative number is `-errno` (`-2` is `ENOENT`); the library does not keep an
`errno` of its own, because a variable per task behind `%fs` is a promise the
program runtime does not need yet.

The formatter is deliberately small: `%d`, `%i`, `%u`, `%x`, `%X`, `%c`,
`%s`, `%p`, `%%`, a width and the flags `-` and `0`, with `l` and `z` for a
long. There is no precision and no `*` width, and no floating point - the
programs are built with `-mgeneral-regs-only`, exactly like the kernel.

The library is compiled into every program image (its objects are linked with
the file of the program): the format is a flat static image and there is no
dynamic linker (ADR 0005). A program that wants the raw door may still use
`programs/prog.h`; `hello`, `crash`, `fork` and `badptr` do, because what they
test is the door, the fault, the fork and the edge - not a library.

### 5. Three programs of the library: `cat`, `echo`, `ls`

The x86-64 port writes seven images to `/bin` at the boot. The three new ones
are the first that are ordinary C programs:

| File | What it exercises |
|---|---|
| `/bin/cat` | `open`, `read`, `write`, `close` on a file of the volume, and `printf` |
| `/bin/echo` | the vector of the arguments and `write`; `-n` is the one option |
| `/bin/ls` | `open` with `ULIBC_O_DIRECTORY` and `readdir` in batches of eight |
| `/bin/spawn` | `execve` from a program, with an environment of its own |

`make x86-smoke` makes `/etc/setting` with the shell, runs
`exec /bin/cat /etc/setting`, `exec /bin/echo one two`, `exec /bin/ls /bin`
and `exec /bin/spawn`, and reads the answers back: a program that ran is a
line on the console that neither the kernel nor the test wrote. `spawn` starts
`/bin/hello` with a vector of its own, and the line `PROGRAM: envp 2,
PATH=/custom` is the proof that the environment of a call is the memory of the
caller copied at the edge, and not the four words of the kernel.

### 6. `exit` is the end of a program

A program ends with `exit(code)` - number 0 of the table - and not with `ret`.
The kernel asks the port whether a program is in flight, the port takes the
task down, and the caller of `execve` waits for it. `_start` calls `exit()`
with the value of `main()`, so a program written as a function of C ends the
way the shell expects. The zero at the top of the stack stays as the guard
for a program that ignores this rule: it faults instead of returning into
nothing, the port reports the fault and the status is 255.

## Alternatives

* **A new system call `execve_env` (number 27).** It would keep number 25
  untouched, but it would also freeze a second shape of the same call for a
  fact - the environment - that POSIX has had in the third argument since the
  beginning. The register is there, the callers that do not use it pass zero,
  and `ksys_execve()` keeps the old wrapper.
* **A file `/etc/environ` read by the port.** The kernel would parse a file of
  the volume at every exec, and a program started before the volume is mounted
  would see nothing. The environment of the system is four words and belongs
  in the kernel until the shell can `export` (docs/debts.md).
* **Link the library of the kernel into the programs.** Impossible: it calls
  `ksys_call()`, which is a function of the kernel's address space, and a
  program of ring 3 does not have that address. The program library is a
  second, much smaller library over the same door, and the shared part is the
  header of the ABI.
* **A dynamic linker and an ELF.** The format is `MOX1` for the reason of ADR
  0005: the smallest machine has ~100 KB of SRAM, and the loader of Linux is
  a chapter of its own.

## Consequences

**Good**

* A program has an environment and an entry ABI that are written down, tested
  and the same for every image of the machine.
* The shell needed no change: `exec /bin/cat /etc/setting` is a line of
  arguments, and the environment is the one of the system.
* `cat`, `echo` and `ls` are ordinary C programs of a library, not
  hand-written calls of `prog.h`; a fourth program costs a file and a line in
  the CMakeLists of the port.
* The library and the kernel agree by construction: both include
  `kernel/ksys.h`.

**Bad**

* The environment of the shell is not the environment of a program: the shell
  has no `export`, so every program sees the four words of the kernel. That is
  a debt and not a design.
* A program has no stdin (descriptor 0 is not the console), no descriptors
  inherited from the shell and no `malloc` in its library. `cat` reads files
  and says so when it is given none; `pipe`, `dup2` and the inheritance are
  the work of another milestone.
* The two vectors are bounded by eight words and 512 bytes each, a number of
  the frame of `execve` and not of a general rule. A program with a long
  environment meets `E2BIG` before the port is asked.
* `printf` is not the `printf` of a standard library: no precision, no `*`, no
  floating point. A program that wants one of them does not have it yet.

## The tests

* `tests/cases/test_exec.c`: the parser clears `envp`; `k_exec_environ()` is a
  vector of words of the shape `NAME=VALUE` with `PATH=/bin` first; a machine
  that cannot run a program answers `-2` whatever vector the image carries,
  and does not write to the structure of the caller.
* `tests/cases/test_ksys_process.c`: an environment of nine words and one that
  does not end are `E2BIG`; a NULL environment and an empty one are the
  answer of the machine (`ENOSYS` on the host), not a refusal of the call; the
  arguments and the environment are bounded apart.
* `make x86-smoke`: `/bin/hello` prints `PROGRAM: envp 4, PATH=/bin`, and
  `cat`, `echo` and `ls` read, write and list through the library, while
  `spawn` starts another program with its own environment and sees it in the
  second program.
