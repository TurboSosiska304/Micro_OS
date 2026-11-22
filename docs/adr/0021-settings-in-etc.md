# ADR 0021 - The settings of a machine are files of /etc

* Status: proposed (after 1.0.0)
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
