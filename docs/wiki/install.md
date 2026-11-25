# Install on a laptop

The full page, with the log of a real boot and the report of a machine that
does not come up, is [`docs/install.md`](../install.md). This page is the same
ten minutes in the short form: the stick, the firmware, the first boot, the
file that survives the power.

Micro OS boots on an **x86-64 laptop with UEFI**: the firmware reads a FAT
partition of a USB stick, the loader starts the kernel, and the disk of the
machine is the **second partition of the same stick** - a MicroFS volume of
24 MB that the kernel mounts read-write through its own USB driver
([ADR 0056](../adr/0056-the-stick-of-a-person-is-a-disk.md)). A file written
there is a sector of the stick and survives the power; a machine whose USB
controller does not come up boots a read-only copy of the disk that travels in
the ESP and forgets everything.

## 1. Write the stick

You need one USB stick of 128 MB or more (**everything on it is erased**) and
`MicroOS-<version>-uefi.img`, 90 MB: from the release, or `make uefi-stick` of
your own tree (`build-uefi/stick.img`). The image is a whole disk - a table and
two partitions - so there is no formatting step and no unpacking.

```bash
lsblk -o NAME,SIZE,MODEL,TRAN      # /dev/sdX is the stick; erasing it is the next line
sudo dd if=MicroOS-2.0.0-uefi.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

## 2. Boot it

1. Boot menu of the firmware (`F12`, `F9`, `F11` or `Esc` at the logo).
2. The entry that says **UEFI** and the name of the stick, not the one without
   the word (that is the BIOS path, and this is a UEFI image).
3. **Secure Boot off**: the loader of this system is signed by the project and
   not by Microsoft, so a firmware that only runs Microsoft's keys refuses it.

A healthy boot says, in this order:

```text
BdsDxe: starting Boot0002 "UEFI ... USB HARDDRIVE ..."
uefi: Micro OS, the loader of a firmware machine
uefi: the picture is 1920x1080, mode 22 of 30, 32 bpp bgrx
x86-64: the boot is uefi, the image at 0x600000
usb0: usb0p2: a volume of this system (mfs), read-only
root: the root is mfs on usb0p2
root: usb0p2 is mounted read-write, the volume is named in /etc/volumes
MicroOS shell ready. Type 'help' for the list of commands.

microos>
```

`root: the root is mfs on usb0p2` is the line that says your stick is the disk
of the machine. `root: the root is mfs on boot0p1` with `read-only` is the
other boot: the machine works on the copy of the ESP and forgets at the power.
The same lines go to COM1 (115200 8N1) for a machine that has a serial port;
a laptop usually has none, and its screen is the console.

## 3. The first commands

The console is a shell and it starts as root: the person in front of the
machine is the machine ([ADR 0044](../adr/0044-a-session-and-the-group.md)).

```text
microos> uname
MicroOS 1.9.0 x86_64 (build ...)
microos> date
2026-11-25 03:47:39
microos> df /
blocks: 15556 of 24510 free (1 KB each)
microos> help
```

`date` is the CMOS of the machine, `df /` the disk of 24 MB, `help` the
commands and then the programs of `/bin`.

## 4. A file that survives the power

```text
microos> write /notes.txt hello from my laptop
21 bytes written to /notes.txt
microos> cat /notes.txt
hello from my laptop
microos> fsck
fsck: the volume is consistent
microos> shutdown
shutdown: the clean end of the machine (off)
...
x86-64: the power goes off (ACPI S5, acpi)
```

The machine switches itself off. Boot the same stick again and read the file:

```text
microos> cat /notes.txt
hello from my laptop
```

## 5. The editor and the programs of the outside world

`/bin/ports` is in `PATH` and carries the programs of the SDK: the editor
**kilo** (BSD-2, not one line changed) and the console set of sbase
(`cksum`, `tee`, `cmp`, `whoami`, `sha256sum`, ...).

```text
microos> kilo /notes.txt
```

`Ctrl-S` saves, `Ctrl-Q` quits. The first picture of the editor takes a moment:
the port draws through the framebuffer of the firmware
([kilo and the terminal](programs.md)).

## 6. The network

The image of this page is the **plain machine**: it has no network stack, and
`netinfo` says `there is no network device` (`wget` is not even a command).
The wire of 2.0 - DHCP, the Intel NIC, `wget`, `ping`, `telnet` - is in the
complete machine of `make x86-all`, and a stick written from that build
(`tools/uefi_stick.py --build build-x86-all`) is the same medium with the
network. The file of the machine is `/etc/network` (`dhcp 0`, `address`,
`netmask`, `gateway`, `dns`, one a line); Wi-Fi is in neither build.

## 7. Two accounts

```text
microos> cat /etc/passwd
root:0:0:/:/bin/sh
guest:7:7:/home/guest:/bin/sh
microos> su - guest
microos> id
uid=7(guest) gid=7(guest) home=/home/guest shell=/bin/sh
microos> logout
logout: the session ends
microos> whoami
root
```

Root becomes anybody without a password on the console; `logout` comes back
to root. The files of an account are the account's, and a file of a group is
read by the group and not written by it.

## When it does not come up

| Symptom | Line of the console | What to do |
|---|---|---|
| the firmware has no UEFI entry of the stick | - | Secure Boot off; the entry with `UEFI:`; the stick is written with `dd` of the whole image |
| Secure Boot refuses the application | - | Secure Boot off in the firmware setup |
| the screen stays black | `uefi: the picture is ...` on COM1 | send that line; the port could not draw the mode |
| `root: ... boot0p1` / `read-only` | no `usb0:` line, or `another system` | the USB controller or the image: the machine is read-only, rewrite the stick |
| `write: /x.txt: input/output error` | `root: boot0p1 ... read-only` | same: the root is the copy in the ESP |
| typing does nothing | `ps2: the keyboard speaks set 2` / `usb: keyboard ...` | a laptop's keyboard is PS/2 through the EC; an external one must be USB; neither line = no keyboard found |
| `shutdown` does not switch off | `this machine has no software switch-off` | press the power button; the data is flushed already |
| a file is gone after the power | `root: ... boot0p1` | the machine booted the image of the ESP, not your stick |
| `kilo: not found in /bin:/bin/ports:/usr/bin` | - | an image older than 1.9; use a 1.9+ release |
| `netinfo: there is no network device` | that line | this image has no network stack; the complete machine of `make x86-all` has it |
| `netinfo: e1000e0, link down` | that line | the cable, or the PHY of the port; Wi-Fi is not supported |
| `dhcp: no server, the fixed address of 1.8` | that line | the LAN has no DHCP server: `kilo /etc/network` with `dhcp 0` and the address |

The full page has the report of a walk through all of this, and
[troubleshooting.md](troubleshooting.md) has the rest of the system.

[Back to the wiki index](README.md)
