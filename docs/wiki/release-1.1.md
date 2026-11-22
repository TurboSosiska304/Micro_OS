# The 1.1 release

`1.1.2` is the second patch: the shell stops at no limit of a directory, a
path with `.`/`..` works, and the long help is not cut. `1.1.1` is the
first patch: the filesystem fixed the data loss of a
directory that grows, and the console refuses `umount /` and `kill 1`; the
volume and the ABI do not change. The notes are in
[the release](../releases/1.1.1.md).

`1.1.0` is the release of the board's last major changes. The ESP32 gets the
settings of `/etc`, the programs in a partition of their own and an update
with a rollback; the kernel gets the first phase of many cores. After this
release the board is a **compatibility port** (ADR 0018): it keeps building
and passing its test, and it receives only the changes that are cheap on this
machine. The checklist is [the notes of 1.1](../releases/1.1.md); the
decisions are ADR 0021 (the settings), ADR 0022 (the layout) and ADR 0012
(many cores).

## A machine is set from the console

The screen, the font and the console are files of `/etc` - `display`, `font`
and `console` - in `key=value` lines, and the boot applies them before the
first frame:

```text
microos> config set display.rotation 90
display: rotation 90, invert 1, brightness 100, theme dark
display.rotation=90
microos> config set font.scale 2
font: scale 2, columns 0, rows 0 (0 is the whole picture)
font.scale=2
microos> config sync
config: 0 file(s) restored, 4 saved
```

`config get`, `config set`, `config list` and `config sync` are the commands;
the NVS keeps a copy, so a format or a flash does not cost the settings. A
broken value falls back to its default with a warning, an unknown key is kept,
and a machine without the hardware says that it does not apply the file rather
than pretending. The full table of keys is in [settings.md](settings.md).

## The layout of the flash changes once

The 8 MB are divided into two application slots of 1.125 MiB, `progs` of
132 KiB and **5.56 MiB of MicroFS for the user** (against 5.94 MiB of 1.0).
`/bin` is the `progs` partition: `ls /bin` lists `cat` and `hello`, and no
image of a program is embedded in the firmware.

**A board of 1.0 loses its volume once**, because the volume moves to a new
offset. Back the files up, then flash with `--erase-volume` (or run `mkfs -y`
at the shell). The NVS - the WiFi credentials, the settings of `/etc` - is not
touched by the flash.

## An update can go back

`tools/ota_usb.py` writes a newer image into the slot that is not running and
resets the board. The bootloader keeps the new slot in `PENDING_VERIFY` until
the shell has proved the boot (the kernel, MicroFS, `/bin`, the self test, the
doctor and the console); a boot that never gets there, or an image whose
signature does not match, is marked invalid and the previous slot starts. An
update writes only the inactive slot and `otadata`; MicroFS and the NVS are
untouched. The network is not an update path in this release.

## The first phase of many cores

Behind `KERNEL_SMP` (off by default) the kernel has a `struct k_cpu` per core,
a run queue per core and a scheduler that never runs a task on two cores. The
host substrate runs two "cores" in `make test-smp`, `make test-tsan` and
`make soak-host`, and x86-64 reads the processor list of the machine and boots
with `-smp 2` in the smoke. The application processors of x86-64 are **not
started** yet; that is the next phase of ADR 0012.

## Upgrading from 1.0

The syscall ABI, MOX1 and the format of a MicroFS volume do not change: a
program and a volume of 1.0 work. Only the partition of the board moves, once,
and only for the ESP32; the x86-64 port and the host substrate need nothing
but the new image.
