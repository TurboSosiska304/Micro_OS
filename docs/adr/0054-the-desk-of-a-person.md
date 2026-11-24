# ADR 0054 - A desk that starts with the machine, and a terminal in a window

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0028 (a UI is a program), ADR 0030 (a window is a library),
  ADR 0044 (a session and the group), ADR 0051 (a terminal for a program),
  ADR 0016 (the entry of a program), ADR 0033 (users and the console),
  `docs/desktop.md`, `docs/plan-2.0.md` (M4), `docs/debts.md`

## Context

1.7 gave the machine a desktop: `/bin/wm` is the one program that owns the
screen, `/bin/edit` and `/bin/fm` are its windows, the pointer is the second
device of the 8042, and the palette is a theme of `/etc/console`. It is not the
desktop of a person yet, for three reasons.

**It does not start.** A person boots the machine, reads the prompt of the
shell and types `exec /bin/wm`. The plan of 2.0 says the desk starts with the
machine, and a release that boots to a prompt is a release that asks a person
to remember a command.

**There is no terminal.** The windows are an editor and a file manager; the
shell of the machine is a task of the kernel (`session_run()` of
`components/config`, ADR 0044) and a window has no way to run it. The one
program that a person of a desktop needs first - a terminal - is missing, and
`docs/desktop.md` names it as the next step.

**The frame is not measured.** `docs/desktop.md` promises that a key costs a
rectangle and not a frame, and nobody has a number for it. The list of the
damage of the server merged rectangles with the box they would make together,
so the clock of the panel at the bottom right touched every window of the desk
and one key repainted the screen. The promise was in the prose, not in the
code.

## The questions

How does the desk start? What is a terminal in this system, where the shell is
a task and there is no pty? What does the panel say, and how is the "damage and
not a frame" of the desktop measured?

## Decision

### 1. The session runs the commands of `/etc/session`

The console session (`session_run()`, ADR 0044) reads one file of `/etc` before
its prompt and runs its lines as commands of the shell, in its own task:

```
# /etc/session - the commands of the console session, one a line
exec /bin/wm
```

* The file is the same kind of thing as the scripts of `/etc/init.d` and the
  settings of `/etc/console`: a file of the volume, and not a number of the
  firmware. A machine without it boots to the prompt of 1.7, which is what
  every smoke test of the shell and of `/bin` reads; a machine with it boots to
  the desktop.
* The commands run **in the session** and not in a task of their own, which is
  the difference from `/etc/init.d`: the desktop is a foreground program of the
  session that started it, the shell waits for it (the `exec` of the shell is a
  wait for the child), and the prompt comes back when the last window closes.
  A daemon of the desk would be a task nobody sees.
* `logout`, a fault of the server and the close of its last window all end in
  the same place: the screen goes back to the console, the session ends or its
  prompt comes back, and the getty of the port makes a new session - which runs
  the file again. A logout is therefore a fresh desktop, which is what a person
  expects of the door of a machine.
* A session of telnet does not read the file: the console of the machine is not
  the terminal of a client (ADR 0051 §1), and a desktop drawn into a socket is
  a desktop of nobody.

The image of a person carries the line; `make x86-disk` keeps it out, because
the gates of the shell, of `/bin` and of the editor drive the machine through
the prompt, and a machine that boots to the desktop has no prompt to type at
until the desk closes.

### 2. The terminal window: a child process and two pipes

There is **no pty** in this system. ADR 0051 gave `/dev/tty` to the task and a
mode to the console, and a pty needs three things this kernel does not have: a
pair of a master and a slave with a number of its own, a way for a program to
make one, and a **shell that is a program**. The shell here is
`kshell_execute()` - a function of the kernel that a session task calls - and a
program of ring 3 cannot call it. A window that ran the kernel's shell would be
a second kernel.

So the terminal of 2.0 is `/bin/term`, a client of the server, with:

* a **child process** of a command line: `fork`, `dup2` of two pipes onto the
  descriptors 0, 1 and 2, `execve` of `/bin/<name>`. The window reads the
  output pipe and puts the bytes in a grid of cells; the keys of a running
  command go to its input pipe, and the arrows, Home, End and Delete are the
  sequences of a terminal of the console;
* a **line of its own** while no command runs: the characters of the keyboard
  are the line, Enter is the command, Backspace and Ctrl+U edit it, and `cd`,
  `clear`, `help` and `exit` are commands of the window because they are
  properties of the process and not of a program;
* **Ctrl+C** is `kill(child, SIGINT)` - the interrupt of a terminal, which is
  what `VINTR` does to the job of a console (ADR 0051 §5) - and Ctrl+Q leaves
  the window, the key of every client of 1.7.

The grid is `components/ui/src/term.c` and not the program: the bytes of a
stream into cells, the line endings, the deferred wrap of the last cell, the
scroll, the sequences of a screen (`ESC[H`, `ESC[K`, `ESC[J`, the erase of a
line), and the damage of the rows that changed. `tests/cases/test_ui_term.c`
holds it on the host, and the program is the picture of the cells and the two
pipes.

What this is **not**: a program of a window does not get a controlling
terminal, `isatty` of the child is false, a program that asks for the size of
its screen gets nothing, and `Ctrl+C` reaches the child as a signal from the
window and not as a byte of a line discipline. A program that insists on a
terminal - `kilo`, `vi`, a shell of somebody else - needs the pty of a later
release; the debt is the row of `docs/debts.md`.

### 3. Alt+T opens the window

The server keeps three keys for itself and 2.0 adds a fourth: Alt+T starts
`/bin/term` in a window of 640x420 at the middle of the desk, raised and with
the keys. The old ones are the left button, Alt+F4 (the close) and the pointer's
motion; a client sees no key of the server (ADR 0030 §1).

The terminal is not a window of the default desk of `/bin/wm`: the two windows
of 1.7 are the desktop that `make x86-vga-smoke` reads pixel by pixel, and a
third window at every start would move the panel and end the desk later. The
person of the booted machine opens a terminal with Alt+T, which is one
keystroke of a desktop that also has an editor and a file manager.

### 4. The clock of the panel

The panel shows the hour of the machine at its right end, in the ink of the
desk:

* the time is the wall clock of the kernel: the CMOS of the machine at the boot
  and the NTP of 1.8 after it (`clock_gettime(CLOCK_REALTIME)`);
* the **format is a key of the theme**, `/etc/console`: `clock=24h` (the
  default, "15:04:05") or `clock=12h` ("3:04:05 PM"), and `utc_offset=+180` for
  the minutes of a zone east of UTC. One file colours the console, the desktop
  and the hour of its panel;
* the second of the machine damages the rectangle of the clock and nothing
  else: the clock is 8 cells of the small font (11 in the twelve-hour format)
  in the right end of the panel, and a still desk paints nothing at all - the
  meter of the server says so.

`components/ui/src/clock.c` is the format without a machine: the line of a
settings file, the seconds of the day of a zone, and the text of the hour. The
keys are the data of `components/settings` with a test on the host, so
`config set console.clock 12h` is a command of the shell like the theme.

### 5. The second size of the font

A machine has one table of glyphs: 95 rows of eight bits, Spleen 8x16. A second
size is not a second table but the **scale of a cell**: `struct ui_font` gains
`scale`, one pixel of a glyph is a square of `scale` pixels, and `ui_font_cell_width`
and `ui_font_cell_height` are the cell of the picture. A scale of 2 is a cell of
16x32 out of the same rows and the same bits.

* `ui/draw.h` draws the title of a window in the cell of the font it is given,
  so a window of the large text has a bar of 37 pixels and a body of 32 a row;
* `/bin/term` switches with Ctrl+T: the same bytes of the grid in a cell of
  16x32, 39 columns and 11 rows of the window instead of 78 and 24. A machine
  of 1280x800 with a person who does not see the small text is a machine with a
  terminal of the large one;
* the host test holds the block of the scaled glyph and the bar of the frame.

A real second *face* (the 16x32 of Spleen, or a bold) is a table of two bytes a
row and a canvas that is not eight bits a cell; the scale is the size a table
of one byte can give, and the debt is written down.

### 6. The frame is measured, and the merge of the damage is fixed

The server counts its frames: the pixels of every rectangle it paints, the
largest rectangle, and the milliseconds of the turn. Two lines say it:

* at the end of the desk, `wm: N frames, the first 1024000 px of 1024000 in X
  ms; Y px after it, Z ms at the worst; the largest WxH`;
* on **F12**, the meter since the last press: `wm: the meter: 3 frames, 40960
  px, the largest 640x16, 2 ms at the worst`. A person presses it, does a
  thing, presses it again, and the two lines are the price of the thing. The
  smoke test measures a typed line and three keys with it.

The numbers were not honest until the merge of the damage list was fixed: the
server grew a rectangle that touched one already waiting, and "touched" was
compared with the edges of the box the two would make together, so two
rectangles anywhere on the screen touched whenever their box spanned the
screen. The clock of the panel at the bottom right made the damage of one key
a frame of 1280x765. `damage_add()` compares the four edges of *its own* two
rectangles now, and a key of the terminal is a row of 16 pixels of its window
(measured: three keys, 3 frames, 40960 px against the 1024000 of the screen).

### 7. The picture of a window is kept by the test

`make x86-vga-smoke` takes its pictures with `screendump` and used to forget
them. `--shots DIR` (default `build-x86/screens`) keeps the picture of the
desk of the boot, of the terminal, of its answer, of the large text and of the
twelve-hour clock: the evidence of the lane is a file a person opens, and the
report of a release names it.

## The tests

* `make test` holds the grid of the terminal (the characters, the wrap, the
  scroll, the sequences, the damage of the rows), the clock of the panel (the
  format of the theme, the zone, the midnight and the noon) and the second size
  of the font (the cell, the block of a glyph, the bar of the frame); and the
  two keys of `/etc/console` through the settings of the machine.
* `make x86-vga-smoke` gains its sixth boot: the machine is told by
  `/etc/session` to run the server, boots to the desk without a typed command,
  shows the hour of the machine and paints only the rectangle of the clock for
  the next second, opens the terminal with Alt+T, runs `cat /etc/setting` and
  reads its answer out of the window, measures a typed line and three keys with
  the meter, compares two screenshots around a key (they differ in the row of
  the key and the clock and nowhere else), stops `cat` with Ctrl+C, puts the
  window in the large text and back, closes the desk and reads the prompt of
  the session; then it sets `clock=12h` of the theme, starts the desk again and
  reads the twelve-hour clock.

## Consequences

**Good.** A machine of this system boots into the desk of a person, and the
desk has the one program a person needs first. The terminal is a client like
the editor: it changes with the volume and not with the firmware, it needs no
kernel interface, and its grid is held without a screen. The compositor is
measured, and the measurement found and fixed a defect of 1.7 that made every
key a screen. The theme colours the console, the desk and its hour from one
file.

**Bad.** The child of the terminal has no terminal: a program that asks
`isatty` gets the truth, and a full-screen program of the console cannot run in
the window. The shell of the window is a small front end of its own and not
`kshell` - two command interpreters of one machine, and the userland one has
fewer commands (a pipe, a redirection and a variable are the next rows of the
debt). The large text is the small one at twice the cell: a person who wants a
wider letter gets a taller one.

**Cost.** `components/ui` gains the grid of a terminal, the clock and the scale
of a font; `/bin/wm` gains the clock of the panel, Alt+T, F12 and the fix of
the damage; `/bin` gains `/bin/term`; `components/config` reads `/etc/session`;
`components/settings` has two keys of the console; `x86-vga-smoke` gains a boot
and about forty checks.

## What this does not do

* It does not add a pty, a controlling terminal, `SIGWINCH`, or a resize of a
  window. A shell of somebody else on this system is the next release's.
* It does not add a panel of applications, a menu, a launcher or a clipboard;
  the keys of the server are Alt+F4, Alt+T and F12 and there is no other door.
* It does not change the console of the kernel, the editor or the file manager.
* It does not promise the desktop to the board: the panel of the stick is the
  console (ADR 0028), and `/bin/wm` needs a picture of 1280x800.

## Related

The desktop of 1.7 is `docs/desktop.md` and the ADRs 0028 and 0030; the terminal
of a program is ADR 0051; the session is ADR 0044; the plan of the release is
`docs/plan-2.0.md` M4. The pty, the shell of the userland and the second face of
the font are the rows of `docs/debts.md` this ADR writes.
