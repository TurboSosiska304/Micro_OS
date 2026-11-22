# ADR 0021 - The settings of a machine are files of /etc

* Status: accepted (1.1.0); the format as built is in the section at the end
* Date: 2026-11-22

## Context

The configuration of the board is already a set of files of `/etc`: a key is
a file, its name is the key and its content is the value, and the port's
key-value store brings the files back after `mkfs -y` (`components/config`,
`config_restore`/`config_persist`). That model works for one value - the name
of the machine, the console - and it does not scale to a screen: a display has
a rotation, a brightness, an inversion and a font size, and a person wants to
edit them together, with a comment, and not to create five files whose names
are long.

The next step of the system is the one a wearable asks for: the settings of
the screen, the font and the rest of the machine are changed through `/etc`,
at a file whose name says which system it is, and the change is applied at the
boot and at the console without a rebuild.

## Decision

### 1. One file per system

A system of parameters is one file of `/etc` whose name is the system:
`/etc/display`, `/etc/font`, `/etc/console`, `/etc/time`, `/etc/net`. The
file is plain text, one `key=value` per line:

```text
# /etc/display
rotation=90
brightness=80
invert=0
theme=dark
```

* `#` starts a comment that runs to the end of the line; an empty line is not
  a line; the spaces around a key and a value are not part of either.
* a key is `[a-z0-9_.-]` and is at most `CONFIG_KEY_MAX` characters; a value
  is at most `CONFIG_VALUE_MAX` characters and is never cut: a file that does
  not fit is refused and the boot uses the defaults.
* a key the system does not know is kept in the file and reported once in the
  log; it is not an error, because a newer release may know it and an older
  one must not destroy it.
* a value the system cannot parse is the default plus a warning, never a
  failed boot.

The files that are **not** a system (`/etc/hostname`, `/etc/setting`) keep the
old model: the name is the key and the content is the value. The list of the
systems is in the tree, not guessed from a name, so a file called `display`
that a person wrote as a key is not taken for a system by accident.

### 2. The store keeps the file, not the key

The key-value store of the port already holds the text of a file under the
name of the file (`config_persist` writes it, `config_restore` brings it back).
A system file is one entry of that store whose value is the whole text. The
same path serves the board (NVS, the flash) and the host (a file per key, the
simulator), and a volume that was formatted again gets its settings back from
the store, as the other files of `/etc` do.

### 3. Applied at the boot and at the console

A system registers an **applier**: a function of the driver that takes the
parsed file and makes it the state of the hardware. The order is the boot
order of the machine: the display and the font before the first frame, the
console before the shell, the network before the first connection. A setting
that a machine cannot honour (a brightness on a machine with no backlight, a
rotation on a terminal) is kept and **ignored with a warning**; the wiki says
which keys each machine honours.

At the console, `config set display.rotation 90` changes the file, applies the
system and, with `config sync`, writes it to the store; a `config get
display.rotation` reads it. The command is the same on the board and in the
simulator; a machine without a display applies nothing and says so.

### 4. Why not one file per key

The old model is one file per key and it stays for the settings that are one
value. A system of five keys would be five files whose names are
`display.rotation`, `display.brightness`, ... and a person editing the screen
would edit five files. A file per system is one file, one comment, and one
`config set` that touches one place. The price is a parser of `key=value`
lines, which is small and bounded, and a rule that says which names are
systems.

## Accepted: the format as built

The decision above is kept as it was taken; this section is what the tree
does, and the numbers a person can rely on. The parser, the registry and the
command are `components/settings`; the appliers belong to the machines (the
panel of the board, the VGA of x86-64) and to no common code.

### The text

* one `key=value` per line, `#` starts a comment, an empty line is skipped,
  and the blanks around the key and around the value are not part of either;
  a `\r` at the end of a line is dropped, so a file written on another machine
  still boots;
* a key is at most 15 characters of `[a-z0-9_.-]`; a value is at most 31
  printable ASCII characters; a line without `=` or with a key that is not one
  is skipped, counted, and named once in the log. A value that is empty or too
  long is skipped the same way, and the key falls back to its default;
* a later line for the same key wins; the file is not rewritten by a read, so
  the earlier line stays for whoever edits it;
* a file holds at most 12 pairs (`SETTINGS_KEYS_MAX`) and 127 characters of
  text (`SETTINGS_TEXT_MAX`). The second number is the size of one value of the
  store of the port (`K_KV_VALUE_MAX` 128): a file the boot accepts is a file
  that `config sync` saves whole. A file over either limit is refused,
  `SETTINGS_ETOOLONG` is the answer, and the defaults of the system are what
  the machine applies;
* the defaults of the three systems are in `g_known` of
  `components/settings/src/settings.c`, and they are the behaviour of the
  machine before the files existed: `rotation=0`, `invert=1`, `brightness=100`,
  `theme=dark`; `scale=1`, `columns=0`, `rows=0` (the whole picture);
  `console` `invert=0`, `theme=dark`.

### The statuses of `settings_*`

| Status | Meaning |
|---|---|
| `SETTINGS_OK` | the call did what it was asked, and the machine took the system |
| `SETTINGS_KEPT` | the file is written, this machine has no applier for it |
| `SETTINGS_EBADKEY` | the key is not `[a-z0-9_.-]` or is too long |
| `SETTINGS_EBADVALUE` | the value is empty, not printable or too long |
| `SETTINGS_ENOSYS` | the name is not one of the systems of the tree |
| `SETTINGS_EKEY` | the system has no such key (a `config set` of a typo) |
| `SETTINGS_EFILE` | the file cannot be read or written |
| `SETTINGS_ETOOLONG` | the file, or the rewrite, is over 127 characters |
| `SETTINGS_ENOTEXT` | the caller's buffer is too small; a value is never cut |
| `SETTINGS_EFULL` | the registry of the appliers is full |

### Writing a file, and why there is no temporary one

The VFS has no `rename`. `config set` builds the whole new text in a buffer of
128 bytes - the comments, the unknown keys and the order of the file are
copied as they are, the line of the key is replaced in place, and a key that
the file does not have is appended - checks the limit, and writes the file
with one `vfs_write()` on a truncating open.

That is **not atomic**: a power cut between the truncate and the end of the
write leaves a short file. The alternatives are worse with the calls there
are. A temporary file (`/etc/.display.new`) and a copy has two windows instead
of one and still no rename to close them; `unlink` + create loses the old file
before the new one exists; and a dot name would leak into the store, whose keys
are the names of `/etc`. With one write the window is the length of one small
file, the next boot reads the lines that are there (the parser skips the cut
one) and `config sync` brings back the text of the store, which is the copy of
the last sync. A checksum of a system file is the next step if a torn file ever
matters; a value is one line of a person and not a record of a database.

### Applying, per machine

| Machine | `display` | `font` | `console` |
|---|---|---|---|
| board (ESP32) | rotation, inversion of the glass, brightness (kept + warning: the backlight is a pin), theme of the dialogue | `scale` 1 or 2 of the 5x7 font, `columns`/`rows` as a cap on the dialogue | `theme`/`invert` of the dialogue |
| x86-64 | none: the file is kept and the load says so | none | `theme`/`invert` as the attribute of every cell of the VGA |
| host | none: the file is kept and the load says so | none | none: the serial console has no attributes |

The order is the boot of the machine:

* the board calls `settings_load_all()` in the init task, after the mount and
  after `config_restore()`/`config_persist()`, and before the task of the shell
  starts the console of the panel - the display and the font are the state of
  the first frame, and the console is the state of the shell;
* x86-64 calls it in `kmain()`, after the filesystem is mounted and before the
  init task exists, so the attribute of the screen is right before the first
  command;
* the simulator calls it in its init task; with no applier registered the load
  names the three systems and keeps the files, which is the "kept with a
  warning" of the decision.

A system that a machine cannot honour is not an error anywhere: the file is
written, `config set` answers `SETTINGS_KEPT`, and the shell prints the value
with the sentence that says it is kept.

### The store, and a format

`config_persist()` already saves every file of `/etc` as one value of the
store of the port; a system file is one value and needs no new call. The rule
of the component - a value is one line and the line ending at its end is not
part of it - means that a restored file loses its last `\n`. The parser reads
the text either way and no key is lost; the tests compare the file with that
one character taken off. `mkfs -y` leaves the store alone, so the next boot
(or `config sync`) writes the files back, and the settings of a machine are
what they were before the format.

## Consequences

**Good**

* The settings of a machine are data of `/etc`, visible with `cat`, editable
  with `write` and `config`, and survive a format through the store.
* A new setting is a line of a file and an applier, not a new call of the
  kernel or a rebuild of the image.
* A machine that cannot honour a setting keeps the file intact for the
  machine that can.

**Bad**

* `/etc` grows a small language (`key=value`, comments) and with it a parser
  that has to be bounded and tested.
* A driver that applies a setting at the boot delays the first frame; the
  order has to be written down and kept.

## Not in this decision

* a schema or a type system for the values: a driver parses its own keys;
* a settings service over the network: later, with authentication;
* a permission model: one user, `/etc` is writable by it (ADR 0005);
* a per-application settings file: a program has its own files, not `/etc`.
