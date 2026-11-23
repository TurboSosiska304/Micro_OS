# ADR 0030 - The window is a library, and the screen has programs

* Status: accepted, implemented
* Date: 2026-11-23
* Related: ADR 0028 (the two devices and the skeleton), ADR 0023 (the screen
  of x86-64), ADR 0016 (the entry of a program), `docs/roadmap.md` (K11, the
  step of 1.5)

## Context

ADR 0028 gave a program two devices - `/dev/fb0` for a rectangle of pixels and
`/dev/input0` for the keys as events - and one program, `/bin/ui`, that draws
a panel, a list of two windows and the log. The skeleton proved the four
things a UI needs: a screen, a key, a frame that does not flicker and a way
back. It also showed the two things that were missing.

The first is the model. A window is a rectangle in an order, with a focus and
a window that takes the keys while it asks; `/bin/ui` had two hard-coded list
rows and a counter, and a second program would have copied the idea. A model
that lives in one program is a model a second program gets wrong.

The second is the keyboard. The scancode set 1 carries the arrows, Home, End,
PageUp, PageDown, Insert and Delete behind the prefix `0xE0`, and the port
dropped that prefix whole from the first release: `/dev/input0` has no event
for a key a text cannot live without. A character was also not a character:
an event names `key.a`, and a program that wants the letter must keep Shift,
Ctrl and Caps Lock and read the layout itself.

## Decision

### 1. The second form of the keyboard

The prefix is a key of the second form and not a byte to drop:

* `components/kernel/include/kernel/input_set1.h` is the table from an address
  of the set 1 to the name of a key, portable and tested on the host: the same
  address without the prefix and with it are two different keys (0x48 is the
  8 of the pad, `E0 48` is Up).
* `enum k_input_key` gains Up, Down, Left, Right, Home, End, PageUp, PageDown,
  Insert and Delete **at the end** (numbers 69 to 78): a number of a key never
  moves, and `K_INPUT_KEY_COUNT` moves with the list. The port pushes them as
  events; the right Ctrl is the modifier of the same form, and the Enter and
  the slash of the keypad still type their character for the shell.
* The layout of a program - the character of a key under Shift, Ctrl and Caps
  Lock - is `ui/keyboard.h`, portable and host-tested: the same US layout the
  console reads, in the names a program sees.

### 2. The window model is a library of the userland

`components/ui` is portable C11 with no door under it and no hardware in it:

* `ui/window.h` - a rectangle, a title, an id and a flag of modal; a screen is
  the array in the order of the paint, the focus and the modal window. The
  operations are the questions an interface has: add, find, raise, remove,
  move the focus, set and clear the modal window, the window that takes the
  keys, the topmost window at a point, and the parts of two rectangles. An
  index moves when a window is raised or removed; the id of the program does
  not, so a program finds its window again.
* `ui/draw.h` - a canvas of pixels that fills, outlines and lays out a line of
  cells of a font the caller gives, and the frame of a window (the paper, the
  border of the focus and the title) into a target of two calls: fill a
  rectangle and copy pixels into it. On the machine the target writes
  `/dev/fb0`; in a test it writes an array of the host.
* `ui/keyboard.h` - the modifiers and the lock as a state, and the character
  of a key under it.

The library allocates nothing and knows nothing of the device; the frame is a
value, and the scratch of a title row belongs to the caller.

### 3. The editor: `/bin/edit`

A file, a cursor, a window. The document is 32 KiB of the bss of the program,
the file is read whole (a longer file is refused, not edited half-way), and
`Ctrl+S` writes it back. The keys are the characters of the keyboard, Enter,
Backspace, Delete, Tab (four spaces), the arrows, Home, End, PageUp and
PageDown; `Ctrl+Q` (or Esc) leaves, and with unsaved changes it asks first in
a **modal** window of the model: `y` discards, `n` or Esc comes back.

The picture is incremental: the program keeps the text of every row it has
drawn and the cell of the cursor, and a key paints the rows whose text or
cursor changed and nothing else. The title bar and the status line are
compared the same way. A line of 1280 pixels is one write of the device.

### 4. The file manager: `/bin/fm`

The directory of the command line (the working directory when there is none),
read with `readdir` of the library - which is the call of the kernel for a
directory and not a second walker - sorted by name, one line each, a slash
after a directory. The arrows, Home/End and PageUp/PageDown move the
selection; Enter opens a directory or hands a file to `/bin/edit` with
`execve`; `d` or Delete opens a modal window that names the object and `y`
calls `unlink`; `q` (or Esc) returns. A file that cannot be removed is a
message of the status line, and a directory is not removed recursively: the
call is `unlink` and the ADR does not pretend otherwise.

### 5. The port's half

`ports/port_x86_64/programs/uiprog.c` is the part that is the machine's and
not the library's: open the two devices, read the shape of the picture, take
the screen with `FBIO_TAKE`, cut a rectangle into strips of whole rows for
`KSYS_MAX_IO`, hold the canvas of one text row, read an event with EAGAIN as
"the user is silent", and give the screen back. `/bin/ui`, `/bin/edit` and
`/bin/fm` link it; `/bin/ui` keeps its own drawing and uses the window model
for its two windows.

### 6. The tests

* The host: `tests/cases/test_ui_windows.c` holds the model (order, focus,
  raise, remove, modal, hit test), the canvas (clipping, the frame, a line
  that ends at its NUL), the frame into a target of the test, and the layout
  of the keyboard; `tests/cases/test_ui.c` holds the numbers of the keys and
  the table of the scancode set 1.
* `make x86-vga-smoke` boots a **copy** of the disk, runs `/bin/edit` on a file
  that is not there, types two lines through the keyboard of QEMU, saves with
  `Ctrl+S`, leaves with `Ctrl+Q`, and boots the same disk a second time where
  `cat /edit.txt` prints the two lines. It runs `/bin/fm /` on a file the
  shell made, reads the title, the names and the selected row out of the
  screenshot, moves the selection, opens the dialog of `d`, answers `n` and
  then `y`, and reads "deleted /zzz" and the shorter list from the screen.
  The keys are `sendkey`: the events come through IRQ1 and the E0 prefix, not
  through the serial console.

## Consequences

**Good.** A program that wants a window has a model and a canvas that are
tested without a screen, and a second program no longer copies the first.
The arrows and the navigation keys are keys of the machine for every program
and every port that speaks the set 1. The editor and the file manager are
files of `/bin` like `cat`: they change with the volume, not with the
firmware, and neither is a privilege of the kernel.

**Bad.** The model is in the memory of every program that links it, the way
the library of a program is one copy per image; a window server would share
it and would be a second kernel to port. The editor holds a file in 32 KiB
and has no undo, no search and no horizontal editing of a line longer than
the window beyond the viewport. The file manager refuses a non-empty
directory and has no copy or rename. There is no mouse, no clipboard and no
compositor.

**Cost.** `components/ui` is three sources and a test; the kernel gains the
table of the set 1 and ten numbers of keys; the port gains the E0 handler and
`programs/uiprog.c`; `/bin` gains two programs; `x86-vga-smoke` gains two
machines (a boot each) and about forty checks.

## What this ADR does not do

* It does not add a mouse, a pointer, or a second input device; the keys are
  the only input of the interface, and the mouse is a debt of `docs/debts.md`.
* It does not add a window server, a compositor or a terminal layer, and it
  does not change the console of the kernel.
* It does not promise the interface on the board: the panel of the ESP32 is
  the console, and `/dev/fb0` is the port of the machine with a picture.
* It does not add a file mode to the editor or the manager: the ABI checks
  the path of a call, and `/bin/edit` writes a file the VFS lets it write.

## Related

The plan of the release is the K11 section of `docs/roadmap.md`; the model is
the step of the screen that 1.5 completes. The programs are `/bin/ui`
(ADR 0028), `/bin/edit` and `/bin/fm`.
