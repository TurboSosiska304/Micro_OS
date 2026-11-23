# The desktop of Micro OS

The machine of x86-64 has a picture, a keyboard and a pointer, and no window
system: since 1.5 a program of the interface (`/bin/ui`, `/bin/edit`,
`/bin/fm`) took the whole screen with `FBIO_TAKE` and painted it itself
(ADR 0028, ADR 0030). 1.7 adds the one program that owns the screen, and the
programs of the interface become its windows:

* `/bin/wm` — the server: the wallpaper, the panel, the pointer, the list of
  the windows, the compositor and the focus.
* a **client** — a program the server started with `fork` and `execve`: a
  buffer of pixels of its own window, a pipe for the events the server sends
  down and a pipe for the requests it sends up.
* `components/ui/client` — the portable half of the contract: the messages of
  `ui/wm_protocol.h` and the client of `ui/wm_client.h`, with a test on the
  host (`tests/cases/test_ui_wm.c`). The port's half is
  `ports/port_x86_64/programs/uiprog.c`, which binds the client to three
  descriptors and a mapping, exactly as `uiprog.c` bound a program to
  `/dev/fb0` before.

## The picture of the desktop

The picture is made of the **roles of a palette** and not of the sixteen
colours of the CGA (`components/ui/include/ui/palette.h`). A role is a colour
written the way a person writes one - eight bits a channel - and narrowed to
the five-six-five of the screen once, when the palette is read. The console of
the kernel keeps its sixteen attributes, because a terminal has sixteen by
definition; the desktop is not a terminal, and a black body under a cyan bar
with a yellow ring said "DOS" to the cleanroom of 1.7.

There are two themes, `ui_palette_dark` and `ui_palette_light`, and the
`theme` of `/etc/console` picks the one the desktop starts with. The file is
the same one the console reads, so one word of `/etc` colours the shell and
the desktop of a machine alike; the programs of the interface read it
themselves through `uiprog_palette`, and a machine without the file gets the
dark theme.

| Part | Where | What |
|---|---|---|
| the wallpaper | the whole picture | a vertical gradient of the theme from `background_top` to `background_bottom`, and a grid of one pixel every 32, both counted from the top of the picture |
| a window | the rectangle the server gave it | the pixels of the client's buffer, a drop shadow of three pixels at its right and below, the square of closing and, when it has the keys, a ring of two pixels of `accent` |
| the title bar | the first 21 pixels of a window, drawn by the client | four pixels of air, the row of the font at six pixels from either side, the title in `title_ink`, a pixel of `border` as the separator, and the body below |
| the panel | the bottom 24 pixels | one pixel of `border` at its top, then a button a window: six pixels of air at either side of a label, sixteen pixels of the panel between two buttons, `surface` for a window that has no keys and `accent` for the one that does |
| the pointer | 12x15 pixels at the point of the mouse | a white arrow with one pixel of black around it |

The **hierarchy of the text** is three inks of one font: `title_ink` for the
bar of a window, `ink` for the body, `ink_dim` for the line of status and for
the label of a button that does not have the keys. The font of this port is
Spleen 8x16 and the tree carries no second size of it, so the weight is not a
second table of glyphs: a title is brighter than a body, the bar is a surface
of its own, and a line of the border divides it from the body - colour, tone
and air, not a bold face.

The server paints *rectangles*: a frame of a client, a motion of the pointer,
a raise and a drag are rectangles of a list of damage, and the loop paints the
rects of the list and nothing else. A window that another one covers is never
read from its file: the compositor asks for the part of it that is visible.
The shadow is a colour of the wallpaper at the row it falls on - the palette's
shadow blended over the gradient - so the bar of depth is the same on the dark
desk and on the light one, and it is drawn under the window and never under
the panel.

The wallpaper's gradient is counted from the top of the picture and not from
the top of the rectangle being painted, so a small damage rectangle and a whole
screen are the same picture; the grid lines are at `UI_PALETTE_GRID_OFFSET`
plus a multiple of the step, and not on the edges of the picture.

The two windows of the default desktop stand **beside each other and not over
each other** - `/bin/edit` at (40, 40) and `/bin/fm` at (700, 200) on the
picture of 1280x800. A window is a rectangle of pixels and the corner of one
covers the corner of another; the lists and the titles of the interface live in
the left of a window, so a window whose corner is covered looks empty while its
frame, its paper and its line of status are still there. The stacking is the
rule of the desktop all the same - a person who puts one window over another
gets the order of the paint, a raise and the occlusion - and the test moves the
two windows together before it clicks.

## The buffer of a window

The pixels of a window are a file of `/tmp` (`/tmp/wm<index>.buf`), made by the
server before the fork, cut to a whole number of pages, and given to the client
as the descriptor 5 of the protocol:

* the **client** maps it with `mmap(..., PROT_READ | PROT_WRITE, MAP_SHARED)`
  and calls `msync` on it before every damage message;
* the **server** keeps the file open and reads the rectangle it needs with
  `lseek` and `read`, in strips of `KSYS_MAX_IO`.

A shared page is *not* shared between two address spaces in this kernel:
`k_mm_fault` gives every mapping its own frame, fills it from the file, and
`msync` writes the present frames back (`kernel/src/mm.c`). Two mappings of one
file are therefore two copies, and the file is the meeting point of the two
programs. The client's write reaches the server because of `msync` and not
without it - `k_mm_msync` writes every page of the range that is present, and
the `dirty` bitmap of the region is only the record of the first store of a
page, because a page that is writable takes its later stores without a fault
(this is why the first window of 1.7 showed its frame and never its text).
The server does not map the file at all: a mapping of its own would cache the
bytes at the moment it faulted, and the only way to make it see a new frame
would be to throw the mapping away and make it again for every message.

`MAP_PRIVATE` is not the mode of a window: a private page is a copy of the file
and nothing comes back out of it.

## The protocol

One message is one `struct wm_message` (`ui/wm_protocol.h`), 60 bytes, native
layout of the machine - the same rule as `struct input_event` and
`struct fb_rect`: both sides are programs of one machine and one compiler, so a
second encoding would be a second thing to test for nothing. A pipe of the
system hands a reader the bytes it has, so the client reads a message only
after `poll` said that the descriptor is readable, and the server reads one
message a turn.

The descriptors are fixed by the contract, because a program cannot choose the
numbers of its own table after an `execve`:

| Number | Direction | What |
|---|---|---|
| 3 | server → client | the events of the window |
| 4 | client → server | the requests of the window |
| 5 | the file | the pixels of the window (`mmap`) |

The variable `MICROOS_WM=1` in the environment says that a program was started
as a window; without it a program is a program of the whole screen.

| Type | Direction | Fields |
|---|---|---|
| `WM_MSG_HELLO` | down, first | `x, y, w, h` of the window and its title — the rectangle the client lays its picture out in |
| `WM_MSG_DAMAGE` | up | `x, y, w, h` of the buffer that changed |
| `WM_MSG_TITLE` | up | `text`: the title of the bar and of the button of the panel |
| `WM_MSG_CLOSE` | up and down | the client is leaving; or the server asks it to leave |
| `WM_MSG_FOCUS` | down | `x` = 1 gained the keys, 0 lost them |
| `WM_MSG_KEY` | down | `code` = a key of `kernel/input.h`, `x` = 1 pressed, 0 released |
| `WM_MSG_MOTION` | down | `x, y` of the pointer in pixels of the window |
| `WM_MSG_BUTTON` | down | `x` = 1 pressed, 0 released, `y, w` = the point of the window |

A type a generation does not know is ignored; a number of a type is never
reused.

The keys of the focused window are the keys of the machine and nothing else:
Shift, Ctrl and Alt arrive as the keys they are, with the press and the
release, so the layout of `ui/keyboard.h` is the whole of what a client needs
to turn them into characters. The server keeps three keys for itself: the left
button of the pointer, Alt+F4 (the close of the window with the keys) and
every pointer motion.

## What the server does

* **A click** on a window raises it and gives it the keys; a click on the body
  of a window that already has them goes to the client as a button event with
  the point in its own pixels.
* **A drag** of the title bar (the first 21 pixels of a window: four pixels of
  air, the row of the font and the separator of `ui/draw.h`) moves it. The
  bar is the handle of a window, so it stays on the picture: the whole height
  of the bar above the panel and at least 64 pixels of its width at either
  side. A window pulled past an edge loses the part of itself that is past the
  edge and never the handle - the sliver of the bar that is left drags it back.
* **The square** at the right of the title bar is the close: the server sends
  `WM_MSG_CLOSE` and the client answers the way it answers its own key of
  leaving (`Ctrl+Q` for the editor, with the question about an unsaved file).
* **Alt+F4** is the same close for the window that has the keys.
* **The panel** puts a window away and brings it back: a window that is put
  away leaves the list of the model and keeps its buffer, its pipes and its
  rectangle, and the button of the panel remains. The square at the right end
  of a button of the panel is the close of that window, so a window whose bar
  is out of reach still has a way out.
* **The buttons act on the release**: the square of a title bar, the label of a
  button of the panel and the square of that button do their work when the
  button of the pointer comes up on the thing that went down. A press that
  slides away is a press of nothing, which is the rule of every button of every
  interface.
* **A client that exits** (a close, a fault, its own key of leaving) is reaped
  with `waitpid(WNOHANG)`; its window leaves the picture, the focus goes to the
  window on top of what is left, and the file of its pixels is removed.
* **The last window ends the desktop**: the server gives the screen back
  (`FBIO_GIVE`), the kernel repaints the grid of the console and the shell that
  started it waits with its prompt.

The server of 1.7 is the whole of the interface: it takes the screen with
`FBIO_TAKE` and never gives it while a window is open. A panic takes the
screen back whatever a program holds (ADR 0028 §2), so a desktop cannot hide a
kernel failure.

## The programs of the interface

`/bin/edit` and `/bin/fm` did not change their logic when the window arrived:
they call the same `uiprog_*` functions, draw the same frame of `ui/draw.h` and
read the same keys. `uiprog_open` asks the environment first: with
`MICROOS_WM` the program is a window (the rectangle of its buffer is its
picture, the events come from the pipe, and a flush sends the damage once a
turn of its loop); without it the program takes the screen as it did in 1.5.

Two things are new in the programs and both are small: a client sends its
title to the server when it changes (`uiprog_title`), and it answers the close
of the server (`uiprog_close_requested`). `/bin/fm` starts `/bin/edit` with
`execve` as before, and the environment travels with the call, so the editor
is a window with the same rectangle as the file manager - a window that becomes
another program is the same window of the desktop.

`/bin/ui` stays the skeleton of ADR 0028: it is a program of the whole screen
and the desktop does not need it.

## The pointer

The mouse of the machine is the second device of the 8042:

* the controller's second port is opened (`0xA8`), its clock and its line
  (IRQ12) are set in the byte of the configuration, and the device is told to
  start from its defaults (`0xF6`) and to report (`0xF4`);
* a packet is three bytes: the buttons, the sign and the overflow of the two
  axes, and the two differences;
* the handler reads them with the keyboard's, because the buffer of the
  controller is one: the status byte says whose byte it is (bit 5);
* a packet whose first byte has not the bit 3 set is not a packet, and the two
  counters of the overflow are the motions the mouse could not express: the
  axis is dropped and the log says so once.

The events are the ones of `/dev/input0`, beside the keys and not in a device
of their own: `key.pointer.x` and `key.pointer.y` (the value is a signed
difference of pixels of the picture) and `key.button.left` (a press and a
release). The numbers are at the end of `enum k_input_key` (79, 80, 81) and the
keys of 1.4 keep theirs. The right and the middle button are not keys of the
contract: a number is not spent on a button nobody asked for.

The point of the pointer is kept inside the picture - the arrow is twelve by
fifteen pixels and the whole of it is on the glass - and the server paints it
again after every turn of its loop that painted anything, so a rectangle of a
window, a raise or a damage of a client cannot paint a cursor away. The pointer
is drawn by the server and not by the driver, and not by a client: a cursor is
a picture of the interface.

## The tests

* `make test` holds the messages and the client on the host
  (`tests/cases/test_ui_wm.c`): the builders, the hello, the clipping of a
  fill and a blit, the damage, the order of `sync` and the damage message, the
  keys, the focus, the close. `tests/cases/test_ui_palette.c` holds the palette
  without a screen: eight bits a channel to five-six-five, the two themes, the
  gradient, the blend of the shadow.
* `make x86-vga-smoke` boots a copy of the disk, runs `/bin/wm`, and reads the
  desktop out of the screenshot and the log of the server: the gradient of the
  wallpaper, the air of a title and of a body, the separator, the two inks of
  the hierarchy, the shadow under a window, the panel with the accent of the
  window that has the keys, the outline of the pointer and the ring of the
  focus; a click of the PS/2 mouse of QEMU raises the editor,
  the ring follows and **the window without the keys keeps its title and its
  text**; `sendkey` types `hello` into the window with the keys and the status
  line of the editor says so; `Ctrl+S` saves the file and the status line says
  it saved five bytes; a sweep of the pointer along the bar of the title
  leaves **no fragment** of a frame in the bar, and neither does the modal of
  the unsaved file; a drag of the title bar moves the window to 140,90; a
  window dragged **past the top-left corner keeps 64 pixels of its bar** and is
  dragged back by that sliver, and one that **hangs past the bottom** is put
  away from the panel and comes back inside the picture; the pointer is drawn
  at both corners of the picture; a **press that slides off a button of the
  panel does nothing**, a click of the button puts the window away and brings
  it back, and **the square of the panel closes it** - the wallpaper comes
  back and the focus returns to what is left; `Alt+F4` closes the file manager
  and the last window ends the desktop, the prompt of the shell comes back,
  `cat /doc.txt` prints the word the keys typed and the picture is the grid of
  the kernel again.
* `make x86-smoke` is the gate of the machine and of `/bin`: the mouse does not
  change a byte of it.

## What is not here

* No clipboard, no resize of a window and no page of settings of the desktop:
  the server is a program of the tree and not a system of settings. The theme
  is the `theme` of `/etc/console` and not a key of the desktop's own.
* A window has one rectangle for its life: a program that wants another one is
  a program that starts again (the editor and the file manager ask for their
  size at the start).
* The compositor reads a covered window only where it is visible, but it reads
  the whole visible rectangle on every damage: a client that says "my buffer
  changed in one row" gets one row read, a client that says "all of it" gets
  all of it.
* One desktop, one picture, one pointer. There is no second screen and no
  session.
* The pointer has one button. The right and the middle button, the wheel and
  the two axes of a touchpad are later numbers of the contract.
* `/bin/wm` is not a service of the boot: a person starts it with `exec
  /bin/wm` and the shell waits for it. A session of `/etc/init.d` is the next
  step.
