# ADR 0051 - A terminal for a program: `/dev/tty`, `termios` and kilo

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0005 (the programs and protection), ADR 0006 (the entry of a
  program), ADR 0016 (the userland), ADR 0023 (the screen of x86-64), ADR 0025
  (copyleft outside the tree), ADR 0028 (a UI is a program), ADR 0033 (users
  and the console), ADR 0044 (a session and the group),
  `docs/porting-programs.md`, `docs/ports-table.md`, `docs/syscall-abi.md`,
  `docs/debts.md`

## Context

`/dev/tty0` has been a device of the devfs since 1.0: `read` waits for a line
and `write` goes to the console of the machine. It is how the session of the
console writes its prompts, how `/bin/uiprog` puts its output where the person
looks, and how `/bin/sqlite3` keeps the console on the descriptors 1 and 2.
What it has never been is a **terminal**: `ioctl` answers `EINVAL` for every
request, `isatty` of the library says no, `tcgetattr` is `ENOTTY`, and the
header `<termios.h>` is a shape with no machine behind it. A program that asks
whether it talks to a person is told the truth: it does not.

That was a limitation with a price of its own. A full-screen program cannot be
a program of this system at all, because every one of them starts the same
way - `isatty(0)`, `tcgetattr(0)`, `cfmakeraw`, `ioctl(1, TIOCGWINSZ)` - and a
library that answers "no" to the first makes the program print a usage line
and leave. The recipe of a port (`docs/porting-programs.md`) promises that a
program of somebody else builds with this SDK and no patch of its source; for
a console program that promise could not even be tested, because the program
would not start.

There is a second half. While a program runs, the caller of `execve` (the
shell) waits for it in steps of five milliseconds and reads the console in
each step, keeping Ctrl+C and dropping everything else: *"a program that runs
does not read a line, and a typeahead nobody asked for is not part of this
contract"* (`ports/port_x86_64/src/port_exec.c`). The sentence was true while
no program read the keyboard. It is false the moment one does, and the keys of
an editor would be eaten by the task that is waiting for the editor.

A third thread: `struct k_terminal` (kernel/console.h) exists since 1.6 for a
session of telnet, and the defect it fixed is the same shape - a dialogue read
from the hardware when it belonged to a socket. A terminal of a program must
be the terminal of *that* task, or the two halves of the system disagree about
who is typing.

## The question

What is `/dev/tty` of this system, what does a program learn from it, what
does it control, and how does a full-screen program that was not written for
this machine draw a screen and read a key?

## Decision

### 1. `/dev/tty0` is the terminal of the calling task

The device keeps its name and its number; what changes is whose terminal it
is. A task with `struct k_terminal` set (a session of telnet) reads and writes
*that* terminal; every other task reads and writes the console of the port.
The rule is the one `k_console_write()` already follows, and it is what makes
`/dev/tty` mean "the thing the person in front of me is looking at" instead of
"the UART of the board".

`/dev/tty` (without the number) is the same device under the name POSIX gives
it: the devfs resolves `tty` to the console of the machine. The two names are
one object, and a program that opens `/dev/tty` and a program that opens
`/dev/tty0` share the same mode, exactly as two `open`s of one terminal do.

### 2. The descriptors 0, 1 and 2 are the console while they are not open

Until 1.8 the rule covered `write` alone: descriptors 1 and 2 are the console
when nothing was opened on them. A terminal extends the same rule to `read`,
`ioctl` and `poll`, because a program that was written for a Unix finds its
terminal on 0, 1 and 2 and never opens `/dev/tty` at all - `kilo` is exactly
that program. A descriptor that *is* open keeps its own meaning: a redirect,
a pipe and a file are not overruled by the fallback.

This is a rule of the kernel (`sys_read`, `sys_write`, `sys_ioctl`,
`poll_entry`) and not of a library, because the same rule has to hold for a
program of another compiler that never includes this tree's headers.

### 3. `ioctl` numbers are the ones of Linux, and the ABI does not move

The call exists: `KSYS_IOCTL` is number 12 and has been since 1.0. What was
missing is the meaning of a request. The requests below take the numbers Linux
gives them and are defined in `kernel/tty.h`:

| Request | Value | Argument | Meaning |
|---|---:|---|---|
| `TCGETS` | 0x5401 | `struct termios *` | read the mode |
| `TCSETS` | 0x5402 | `const struct termios *` | set it now |
| `TCSETSW` | 0x5403 | `const struct termios *` | set it, drain first (same as `TCSETS`) |
| `TCSETSF` | 0x5404 | `const struct termios *` | set it, flush the input first |
| `TIOCGWINSZ` | 0x5413 | `struct winsize *` | the size in cells |
| `TIOCSWINSZ` | 0x5414 | `const struct winsize *` | accepted, ignored |
| `FIONREAD` | 0x541B | `int *` | bytes waiting in the input |

No system call is added, no number is renumbered, and nothing of the frozen
ABI changes: a request is data of the call. A device that does not know a
request answers `EINVAL`, as before (ADR 0028).

`struct termios` is the Linux one, field for field, and it crosses the call as
it is: four `k_u32_t` flags, `c_line`, and `K_TTY_NCCS` (19) bytes of
`c_cc`, 36 bytes with no padding. The numbers of the flags and of the indices
of `c_cc` are Linux's as well: a program does not compare them to constants, it
does `c_lflag &= ~(ECHO | ICANON | ISIG)` and `c_cc[VMIN] = 0`, and the names
are the contract, not the values. The values are Linux's so that a source that
does hardcode one - a few old ones do - is not silently wrong.

### 4. The mode lives with the terminal, not with the descriptor

`termios` is a property of the device, as it is on a real machine: two
programs that open one terminal share its mode, and a program that changes it
and exits changes it for the next one. This system has at most one console and
one terminal per session, so the mode is a small structure: `g_console` for
the console of the port, and one per task for a task that has a terminal of
its own (`struct task::tty_state`, made on the first `TCGETS` and freed with
the task).

The console is put back to canonical with echo when the task that last changed
it goes away (`k_tty_task_exit`), so a program that dies in raw mode does not
leave the keyboard of the machine raw. A terminal that a person does not type
into costs one block of the allocator.

### 5. What the mode means

The bits this system implements:

* **`ICANON`** - the line discipline: the input is a line at a time, `VERASE`
  (and `\b` and 127) erases a character, `VKILL` erases the line, `VEOF` at
  the start of a line is the end of the file (a read of zero) and in the
  middle of one ends the line, `VINTR` sends `SIGINT` to the job of the reader
  and throws the line away. The line is at most `K_TTY_LINE_MAX` (128) bytes;
  the rest of an overlong line is dropped to the newline, the way the shell
  drops an overlong command.
* **`ECHO`** - the tty repeats what the person types, because a screen and a
  UART do not. `ECHOE` erases with `\b \b`. A terminal of telnet that asked
  for the echo of the server (RFC 857) is not echoed twice:
  `k_console_terminal_echo()` decides, and it is the question the session of
  the console already asks.
* **`ISIG`** - `VINTR` is a signal and not a byte. With `ISIG` off - raw mode
  - Ctrl+C is the byte 0x03, which is what `kilo` reads as "ignore me".
* **`VMIN`/`VTIME`** in raw mode, in the four combinations POSIX defines: a
  read of `VMIN` bytes that blocks, a read that returns as soon as a byte is
  there or after `VTIME` tenths of a second, and the two together. A read of a
  terminal that is gone answers zero, which is the end of the file.
* **`OPOST`, `ONLCR`, `ICRNL`, `IXON`, `CS8`** are accepted and carried in the
  structure. `ICRNL` is applied (a `\r` of the keyboard is a `\n` of the
  line). `ONLCR` is not, because the console of this system already puts the
  cursor at the start of the next line on `\n`, and a second translation would
  turn one line into two. `IXON` is not, because the console has no flow
  control to stop.
* **The speeds** (`cfsetispeed`, `cfsetospeed`, `c_cflag`'s `B9600`) are
  no-ops: the UART of the board and the serial line of x86-64 are programmed
  once at the boot, and a program that asks for 19200 is not lying to itself
  by being ignored - the line is what the machine made it.

### 6. The size of the terminal

`TIOCGWINSZ` answers the size of the screen in cells, through a new question
of the port, `k_port_console_size()`:

| Machine | Rows x columns |
|---|---|
| x86-64, text mode of the VGA | 80 x 25 |
| x86-64, framebuffer (ADR 0023) | 160 x 50 (or what `font.line_gap` left) |
| board | 80 x 25 of the console |
| host, a real terminal | `TIOCGWINSZ` of the process |
| null port | 80 x 25 |
| a terminal of telnet | 80 x 25 |

A terminal of telnet has no negotiated size (there is no NAWS in the part of
the protocol this system speaks), so 80 x 25 is the answer and the debt is
written down. `TIOCSWINSZ` is accepted and ignored: nothing may resize the
machine from a program, and a program that sets a size gets the one it set
only until the next question.

### 7. The console of x86-64 reads the sequences of a screen

A full-screen program draws with escape sequences, and the screen of x86-64
read exactly three of them: `ESC[2J`, `ESC[H` and the SGR colours. The console
grows the rest of the set a program of this shape uses, in
`ports/port_x86_64/src/console.c`:

* `ESC[<row>;<col>H` and `ESC[f` - put the cursor at a cell (1-based, a
  missing number is 1, a number past the edge is the edge);
* `ESC[A`, `ESC[B`, `ESC[C`, `ESC[D` - up, down, right, left, `ESC[<n>C` and
  its three siblings by `n` cells, clamped to the grid;
* `ESC[G`, `ESC[d` - the column and the row;
* `ESC[K` - erase from the cursor to the end of the line, `ESC[1K` from its
  start, `ESC[2K` the whole line;
* `ESC[J` - erase from the cursor to the end of the screen, `ESC[1J` from its
  start, `ESC[2J` all of it;
* the SGR sequences as before (ADR 0023), including the reverse of `ESC[7m`
  that a status bar is drawn in;
* every other final byte ends the sequence and leaves no trace, which is what
  makes `ESC[?25l` of a program that hides the cursor harmless.

The scroll stays the one the console had: a write past the last row scrolls the
grid by one line. A scroll *region* is not there, and a full-screen program of
this tree does not use one.

The serial line is the log, and the log of a screen is text: `COM1` receives
the same bytes with **every** CSI sequence removed, not the SGR ones alone. A
person at the serial console of a machine running an editor reads the lines
the editor wrote and not the bytes that placed them, and the smoke test reads
a transcript and not a terminal.

### 8. The keys belong to the program that reads them

The caller of `execve` waits for the program and reads the console for Ctrl+C.
That read now stops while a program is reading the terminal itself: the tty
keeps a `reading` flag for as long as a read of the console is blocked, and the
waiter asks `k_tty_console_claimed()` before it polls. A key typed at an editor
is the editor's byte; Ctrl+C is a byte too while `ISIG` is off, and the
program's own `read` is the only reader of it.

The flag is a property of the console, so a session of telnet never claims the
keyboard of the board, and a program that dies in the middle of a read releases
it with the rest of its state.

### 9. The library: `<termios.h>`, `<sys/ioctl.h>`, `isatty`

`components/libc/user` grows the shape a portable source includes:

* `<termios.h>` - the flags, the indices, `struct termios`, `tcgetattr`,
  `tcsetattr`, `cfmakeraw`, `cfgetispeed`, `cfsetispeed`, `cfgetospeed`,
  `cfsetospeed`, `cfmakeraw` and `tcflush`;
* `<sys/ioctl.h>` - `struct winsize`, `TIOCGWINSZ`, `TIOCSWINSZ`,
  `FIONREAD`, `TCGETS`, `TCSETS`, and `int ioctl(int fd, unsigned long
  request, ...)`;
* `isatty(fd)` - `ioctl(fd, TCGETS, &t)` and the answer, which is true for the
  console on 0, 1, 2, for `/dev/tty0` and for `/dev/tty`, and false for
  everything else.

The calls keep the shape of POSIX: zero or -1, never a negative errno. The
`0/1/2` fallback is the kernel's, so `tcgetattr(0, &t)` works in a program
that never opened anything and `tcgetattr(3, &t)` on a file answers `ENOTTY`.

### 10. The proof: kilo, built without a patch

`tests/ports/kilo/kilo.c` is the editor of Salvatore Sanfilippo
(`antirez/kilo`, BSD-2-Clause), copied byte for byte with its `LICENSE` and
the sha256 of the upstream in `tests/ports/upstream.txt`. It is built by the
SDK alone (`tools/ports_build.sh`), like the thirteen tools of sbase, and it is
a file of `/bin/ports` of the volume of `make ports-smoke`.

`make tty-smoke` is the half of the gate that a transcript can see: the
machine boots, `/bin/ports/kilo /tmp/k.txt` opens a file that is not there,
a line is typed, Ctrl+S saves it, Ctrl+Q leaves, and `cat /tmp/k.txt` shows
the line. A second program of the gate, `tests/ports/ttyprobe.c`, prints the
mode before and after `cfmakeraw` (`TCGETS` on the console, an `ioctl` on the
descriptor that is not open) and the size of the screen, so the run is also the
proof of the ioctl calls themselves.

The license travels: `tests/ports/kilo/LICENSE`, a row in
`docs/licensing.md`, a copy in the bundle.

## What this does not do

* **A window size that changes.** `SIGWINCH` is not a signal of the ABI; the
  library defines the number so that a source compiles, and `signal()` answers
  `SIG_ERR` for it. An editor that is told about a resize is told about
  nothing.
* **The arrow keys of the console of x86-64.** The PS/2 decoder gives the
  extended keys to the interface (`/dev/input0`, ADR 0028) and not to the
  character stream, so an editor moves with the keys it has: the letters, the
  Enter, the Backspace, and its own Ctrl combinations. `kilo` opens a file,
  edits it, saves it and leaves; the arrows, Home and End are a row of
  `docs/debts.md` and a change of the decoder, not of the terminal.
* **More than the sixteen colours of the console.** A cell of the screen has
  an attribute, and `ESC[38;5;...m` is not one of them.
* **A scroll region and the insert/delete of a line** (`ESC[r`, `ESC[L`,
  `ESC[M`). `kilo` repaints the screen instead.
* **A real windowing of sessions.** The terminal of telnet has no size
  negotiation and its `get` cannot be asked whether a byte waits without
  waiting for one, so `VMIN`/`VTIME` of a session over the wire is the
  blocking read of the socket.
* **`vi`, `neatvi`, `nvim`.** They are the next names of this gate and they
  ask for more than kilo: a scroll region, the arrows of the console, a termcap
  or `terminfo` of the machine.

## Consequences

* A program of somebody else can be a console program on this system, and the
  recipe of a port is true for the first time for the family that matters most
  to a person in front of the machine.
* The console of x86-64 is a terminal and not a text sink. The three sequences
  of `clear` are a subset of what it now reads, so nothing that worked stops
  working, and the smoke test of the screen (`make x86-vga-smoke`) is the check
  that the old paths still draw the same.
* A blocking read of the console is a task that sleeps on
  `K_WAIT_CHANNEL_CONSOLE`; a key wakes it. The cost of a person typing is one
  wakeup, as it was for the shell.
* A program that asks `isatty()` now gets the truth, and a program that
  behaves differently on a terminal behaves differently here. The shell of
  SQLite is the first case: it drew the box of a terminal instead of the list
  of a batch, and its table has no glyph in the font of the console, so
  `/bin/sqlite3` of the port asks for `.mode list` and for an input that is not
  a terminal (`ports/port_x86_64/programs/sqlite3.c`). A port of a program that
  asks the question is a port that has to read the answer.
* `docs/debts.md` keeps the rows this ADR writes: `SIGWINCH`, the arrows of the
  console, `NAWS` of telnet, the colours past sixteen and the region of a
  screen.
