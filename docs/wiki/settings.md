# The settings of a machine

A few settings of a machine are data of `/etc`, and the screen, the font and
the console are the first of them: one file per **system** of parameters, with
`key=value` lines and comments. A person edits them with `cat`, `write` and
`config`; the boot applies them before the first frame; and the store of the
port keeps them across `mkfs -y`. The decision and its reasons are
[ADR 0021](../adr/0021-settings-in-etc.md).

## What a system is

`/etc/display`, the file of the screen of a machine:

```text
# the screen
rotation=90
invert=0
brightness=100
theme=dark
```

* `#` starts a comment that runs to the end of the line; an empty line is not
  a line, and the spaces around a key and around a value are not part of
  either.
* a key is `[a-z0-9_.-]` and at most 15 characters; a value is at most 31
  printable characters and is never cut.
* a later line of the same key wins. The file is not rewritten by a read, so
  the earlier line stays for the person who edits it.
* a line without `=` or with a key that is not one is skipped and counted;
  the boot names the number of skipped lines once in the log.
* a value the system cannot read (a number where a word belongs, 400 where
  100 is the ceiling) does not take effect: the machine uses the **default of
  the key** and writes a warning to the log. The file is not rewritten, so the
  text stays what the person typed; `config get` shows that text, not the
  default the machine is using.
* a key the system does not know is **kept** in the file and named once in the
  log: a newer release may know it, and this one must not delete it.
* a file holds at most 12 pairs and 127 characters, because it is one value of
  the store of the port. A longer file is refused and the defaults are used.

The three systems of the tree:

| System | File | Keys | Defaults |
|---|---|---|---|
| `display` | `/etc/display` | `rotation` 0/90/180/270, `invert` 0/1, `brightness` 0–100, `theme` dark/light | 0, 1, 100, dark |
| `font` | `/etc/font` | `scale` 1/2, `columns` 0–99, `rows` 0–99, `line_gap` 0/2/4 | 1, 0, 0, 0 |
| `console` | `/etc/console` | `invert` 0/1, `theme` dark/light, `ink`, `paper` | 0, dark, theme, theme |

`columns=0` and `rows=0` mean the whole picture: the console cuts the dialogue
by the room the panel has at the size of the font. A positive value is a cap,
so a person can keep the dialogue narrow on a wide screen. `line_gap` is the
gap between two lines of the framebuffer of x86-64, in pixels: 0 is the cell of
16 rows itself, 2 and 4 are 18 and 20 pixels a line, which is 50, 44 and 40
rows of a picture of 800. A value the key does not have is 0 and a warning.

`ink` and `paper` are the pair of the plain text of the console, and they are
read after `theme` and `invert`, so a colour a person named wins over them.
The value is a name of the palette of the screen - `black`, `white`, `grey`,
`green`, `amber`, `red`, `blue`, `cyan`, `magenta`, `yellow` - or a number of
RGB565 written `0xRRRR`, such as `0xF800` for the pure red. The word `theme`
means the pair the theme chose and is the default of both keys. A value that
is neither is the default and a warning; a colour that is not one of the
sixteen of the VGA is rounded to the nearest of them on the text mode and
painted as it was written on the framebuffer.

The defaults are the behaviour of the machine before the files existed, so a
volume that never had `/etc/display` boots exactly like a release that did not
have this feature.

## What each machine honours

| Key | Board (ESP32) | x86-64 | Host |
|---|---|---|---|
| `display.rotation` | the picture is turned, before the first frame | kept, not applied | kept, not applied |
| `display.invert` | INVON/INVOFF of the glass | kept, not applied | kept, not applied |
| `display.brightness` | kept and a warning: the backlight is one pin, with no duty cycle to dim it | kept, not applied | kept, not applied |
| `display.theme` | the colours of the dialogue | kept, not applied | kept, not applied |
| `font.scale` | 2 doubles the 5x7 font and the cells of the console | kept, not applied (the font of the PC is 8x16) | kept, not applied |
| `font.columns`, `font.rows` | a cap on the dialogue | kept, not applied (the grid is the picture) | kept, not applied |
| `font.line_gap` | kept and a warning: the cell of the panel has no gap | the gap between two lines of the framebuffer, 0/2/4 px | kept, not applied |
| `console.theme`, `console.invert` | the colours of the dialogue | the pair of every cell of the plain text of the screen (the framebuffer's two colours, or the attribute of the VGA text mode); a word a command coloured keeps its own | kept, not applied |
| `console.ink`, `console.paper` | kept and not applied: the panel has its own dialogue | the ink and the paper of the plain text of both screens, a name of the palette or a number of RGB565 | kept, not applied |

`display.theme` and `console.theme` are the same picture on the board and the
console is read after the display, so `/etc/console` wins when the two
disagree. On x86-64 the theme is the pair of the screen: light grey on black
for `dark`, black on light grey for `light`, and `invert` swaps the two (the
text mode logs the attribute, `7` or `112`; the framebuffer logs the screen it
painted). `ink` and `paper` are read after the theme, so a colour a person
named wins over the two; the word `theme` is the default. The host has no
screen of its own: the load names the systems it cannot apply and keeps the
files.

## The commands

`config` without a word still lists the keys of the store of the port and
their values, and `config sync` still restores and saves the files of `/etc`.

```text
microos> config list
display  /etc/display  (not applied here)
font     /etc/font  (not applied here)
console  /etc/console
microos> config get display.rotation
display.rotation=0
microos> config set console.theme light
console: theme light, invert 0, the attribute of the screen 112
console.theme=light
microos> config get console.theme
console.theme=light
microos> config set display.invert 0
display.invert=0 (kept: this machine does not apply display)
microos> config list console
console.invert=0
console.theme=dark
console.ink=theme
console.paper=theme
microos> config set console.ink 0xF800
console: theme dark, invert 0, the screen the framebuffer of 1280x800, 16 bpp, 160x50 cells
console.ink=0xF800
microos> config list display
display.rotation=0
display.invert=0
display.brightness=100
display.theme=dark
```

* `config get <system>.<key>`: the value of the file, or the default of the
  system when the file does not have the key. An unknown system or key is a
  sentence with the names that exist, not a silence.
* `config set <system>.<key> <value>` (or `config set <system> <key> <value>`):
  the line of the file is replaced or appended, the file is written whole and
  the system is applied at once. A key the system does not know is refused, so
  a typo does not reach the file.
* `config list [system]`: the systems and their files, or every key of one
  system with its value.
* `config sync`: restore the missing files from the store and save the ones of
  `/etc`. The boot does the same on every start.

A machine that cannot honour the system writes the file, logs the reason and
answers `SETTINGS_KEPT`; the shell prints the value with the sentence that
says it is kept. That is not an error: a card with a `/etc/display` of another
machine is a card that boots.

A value that is out of the range of its key is the other case: the file keeps
the text, the machine uses the default of the key, and the boot log names the
default. `config set display.rotation 400` writes `rotation=400` to the file,
but the applier of the display reads it as an invalid rotation and uses `0`;
`config get display.rotation` therefore answers `400` until the file is edited,
while the screen stays at the default.

## When it is applied, and by whom

The applier belongs to the machine and is registered with its system:

| Machine | Where the applier is |
|---|---|
| board | `components/drivers/src/drv_display.c` (`display`, `font`, `console`) |
| x86-64 | `ports/port_x86_64/src/console.c` (`console`), `src/fb.c` and `src/vga.c` are the two screens |
| host | none: the load names the three systems and keeps the files |

The order is the boot of the machine. The board loads the systems in the init
task, after the volume is mounted and the files are restored, and before the
task of the shell starts the console of the panel: the display and the font
are the state of the first frame. x86-64 loads them in `kmain()` after the
filesystem is up, so the attribute of the screen is right before the first
command. Since `1.2.0` the console of x86-64 is the framebuffer of 1280x800
(or the text mode of 80x25 when the card has no interface), and `/etc/console`
and `/etc/font` are applied to whichever screen the port chose. The simulator
loads them in its init task, and a run with a screen file still has no
applier: the console of the host is a serial port.

## A format, and the store

The files are the settings of `/etc` like any other, so the store of the port
keeps them: `config sync` (and the boot) saves the text of every file under its
name, and a volume that was formatted again gets the files back. `mkfs -y`
takes the volume and not the store. `idf.py erase-flash` takes both.

The write of a `config set` is one write of the whole text and the VFS has no
`rename`, so it is not atomic: a power cut in the middle leaves a short file,
the next boot reads the lines that are there, and `config sync` brings back the
text of the last sync. That trade is written down in
[ADR 0021](../adr/0021-settings-in-etc.md).

## What it does not do

* **No dimming of the panel.** The backlight of the board is a pin: on or off.
  `brightness` is kept for a panel that can and is reported once per boot.
* **No screen on the host.** A machine that cannot honour a system keeps the
  file and says so; nothing is silently dropped.
* **No schema and no type system.** The applier of the machine reads its own
  keys; the tree knows the names of the keys so that `config set` can refuse a
  typo, and nothing more.
* **No settings of a program.** A program has its own files, not `/etc`.
* **No per-key write.** A `config set` writes the whole file, and the nine
  other keys are copied as they are.

The tests are `tests/cases/test_settings.c` on the host (the parser, the
rewrite, the defaults, the appliers, `set`/`get` and a format through the
store), the `config` commands of `make x86-smoke`, and the device test of the
board (`config set display.rotation 90`, the log of the display, a reboot).

[Back to the wiki index](README.md)
