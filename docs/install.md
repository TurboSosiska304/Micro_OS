# Install Micro OS on a laptop

This page takes a machine that has never seen Micro OS to a working shell, a
file that survives the power, and a second boot that finds it. It is written
for a person who has never seen this tree: every step is a command you type,
and every step shows what the machine answers. Ten minutes of reading and
typing, most of them waiting for the stick.

The machine is an x86-64 laptop with a **UEFI** firmware and a **USB port**.
The board (the M5Stack StickC Plus2) is another machine with another page:
[`docs/ports/esp32`](ports/esp32/README.md).

## What you need

| | |
|---|---|
| the medium | one USB stick of 128 MB or more. **Everything on it is erased.** |
| the image | `MicroOS-<version>-uefi.img`, 90 MB, from the release, or built by `make uefi-stick` |
| the machine | x86-64, UEFI (not a BIOS-only machine of 2008), Secure Boot **off** |
| the tools | `dd` (any Linux) to write the stick; to make the image yourself: `cmake`, `ninja`, `gcc`, `python3`, `clang` and `lld`, `sfdisk`, `mkfs.fat` (dosfstools) and `mmd`/`mcopy` (mtools). OVMF is only for running it in QEMU |

The image is a whole disk, not a file to copy: a partition table with two
partitions. The first is the **ESP** the firmware reads (the boot program and
the kernel); the second is a **MicroFS volume of 24 MB**, and that one is the
machine's disk - the system mounts it read-write and a file you write is a
sector of the stick ([ADR 0056](adr/0056-the-stick-of-a-person-is-a-disk.md)).
A machine whose USB controller does not come up boots the copy of the disk
that travels in the ESP, read-only: it answers, and it forgets at the power.

## 1. Write the stick

Find the name of the stick **first**. `/dev/sdX` is the stick, and `/dev/nvme0n1`
or `/dev/sda` is the disk of your laptop - the next command erases whatever
it is given:

```bash
lsblk -o NAME,SIZE,MODEL,TRAN
```

Unmount anything the desktop mounted from the stick, then write it:

```bash
sudo dd if=MicroOS-2.0.0-uefi.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

No `mkfs`, no `parted`, no unpacking: the image already carries the two
partitions, and a `dd` of the whole image is the whole preparation. When `dd`
says it wrote 90 MB, the stick is ready.

## 2. Boot it

1. Put the stick in the laptop and switch the machine on.
2. Open the boot menu of the firmware (on most laptops `F12`, `F9`, `F11` or
   `Esc` at the logo; some want `Fn` held).
3. Choose the entry with **UEFI** and the name of the stick - `UEFI: SanDisk
   ...`, `UEFI: USB HDD` - and not the entry without the word (that one is the
   BIOS path, and this image is an image of UEFI).
4. If the firmware refuses with a **Secure Boot** message, or the menu has no
   UEFI entry of the stick, turn Secure Boot off in the firmware setup
   (`Security` → `Secure Boot` → `Disabled`) and try again. The boot program
   of this system is signed by the project and not by Microsoft, so a
   firmware that only runs Microsoft's keys does not run it.

The screen is the picture of the firmware. The same lines go to the serial
port of the machine (COM1, 115200 8N1) for a machine that has one - a desktop, a
dock with a serial port, a firmware that redirects its own console there. A
laptop usually has none, and its screen is the only console; when the screen
says nothing, the last line of the table in "When something does not work" is
the one to ask about.

### What you should see

First the firmware's own line, which says it found the stick (the line names
your stick and your USB port; this one is the emulated machine of the
project):

```text
BdsDxe: loading Boot0002 "UEFI QEMU QEMU USB HARDDRIVE ..." from PciRoot(...)
BdsDxe: starting Boot0002 ...
```

Then the boot program of the system (the loader), which reads the kernel and
the disk out of the stick:

```text
uefi: Micro OS, the loader of a firmware machine
uefi: the picture is 1920x1080, mode 22 of 30, 32 bpp bgrx
uefi: the kernel is 648 KB
uefi: the disk is 16384 KB
uefi: the boot services are gone, the kernel takes the machine
```

Then the kernel. Six lines of them are the promise of the medium, and they are
the ones to read when something does not work:

```text
x86-64: the boot is uefi, the image at 0x600000
xhci: port 1: a device at speed 4 (super), PORTSC 00021203 -> 00001203
usb0: usb0p2: a volume of this system (mfs), read-only
root: the root is mfs on usb0p2
usb0: the writes of usb0p2 are open: the volume is named in /etc/volumes
root: usb0p2 is mounted read-write, the volume is named in /etc/volumes
```

The first says the firmware path worked; the `usb0:` lines say the port found
the stick and read its table; `root: the root is mfs on usb0p2` says the
**second partition of your stick is the disk of the machine**; and the last
says the writes are open, which is why a file you write survives. A machine
that prints `root: the root is mfs on boot0p1` and `read-only` instead has
booted the copy of the disk that travels in the ESP: it works and it forgets
everything at the power (see "When something does not work").

The boot ends with the shell:

```text
MicroOS 2.0.0 (build ...)
board: x86_64 — the x86-64 machine of this port
arch: x86_64, compiler: gcc ...
port: x86_64, contract 1.14
[ ok ] sched
...
x86-64: the programs come from usb0p2:/bin, the volume is mfs
MicroOS shell ready. Type 'help' for the list of commands.

microos>
```

`MicroOS 2.0.0` is the version of the image you wrote - an image built from
the sources of this tree says the number of those sources, and `uname` prints
it again.

## 3. The first commands

The console is a shell and it starts as **root**: the person in front of the
machine is the machine, and there is no password on the physical console
([ADR 0044](adr/0044-a-session-and-the-group.md)). Four commands to see what
you have:

```text
microos> uname
MicroOS 2.0.0 x86_64 (build ...)
microos> date
2026-11-25 04:27:05
microos> ls /
bin
dev
etc
home
proc
tmp
microos> df /
blocks: 15556 of 24510 free (1 KB each)
inodes: 192 of 319 free
```

`date` is the clock of the machine: the firmware's CMOS chip, read at the boot.
`ls /` is the tree of the system: `/bin` has the programs, `/etc` the
configuration, `/home` the accounts' directories. `df /` is the disk: 15 MB
free of 24 MB on a stick just written (the numbers are the ones of the stick
this page was written with; the tree of a release moves by a few files).

`help` lists every command of the shell and then the programs of `/bin`, and
`help <name>` describes one:

```text
microos> help
command   what it does
...
write     write a file
df        the usage of the filesystem
mount     list, mount or unmount the filesystems
fsck      check the volume, or repair it with -y
date      the date and the time of the machine (UTC)
...
programs in PATH:
  hello crash fork ... zcat pcre2grep lua zstd ... kilo ...
```

## 4. A file that survives the power

This is the whole point of the medium. Write a file, read it, look at the
disk, then switch the machine off and on again.

```text
microos> write /notes.txt hello from my laptop
21 bytes written to /notes.txt
microos> cat /notes.txt
hello from my laptop
microos> ls -l /notes.txt
-rw-r--r--    1 root     root             21 Nov 25 04:27 /notes.txt
microos> fsck
fsck: 114 file(s), 14 directory(ies), 15555 of 24510 block(s) free
fsck: the volume is consistent
```

`fsck` is the check of the volume: `the volume is consistent` is the healthy
answer. Then the clean end:

```text
microos> shutdown
shutdown: the clean end of the machine (off)
power: syncing the volumes and flushing N disk(s)
power: the volumes are synced, the disks are flushed
power: no new session starts, the tasks are asked to end
power: every task has ended
power: syncing the volumes and flushing the disks once more
power: the mounts are gone
x86-64: the power goes off (ACPI S5, acpi)
```

The machine switches itself off (ACPI S5). Two last lines can follow the one
above and neither is a failure: `acpi: S5 on the port 0x...` names the port of
the sleep, and `the S5 request did not take, the ports of the emulators are
tried` is the emulator of this project doing it another way. A firmware whose
tables name no power-off port cannot be switched off by software: the last
line is then `x86-64: this machine has no software switch-off; press the power
button`, and you press the button - the data is already on the stick, because
`shutdown` flushed it before it tried.

Switch the machine on again, boot the stick, and read the file:

```text
microos> cat /notes.txt
hello from my laptop
```

If this line prints, the medium is the disk of the machine and the release is
what it promises.

## 5. The editor, and the programs of `/bin/ports`

`/bin/ports` holds the programs of the outside world that the release builds
with its own SDK: the editor **kilo** (BSD-2, not one line changed), and the
thirteen tools of sbase (`cksum`, `tee`, `cmp`, `whoami`, `sha256sum`, ...).
They are commands of the machine like the others, because `/bin/ports` is in
`PATH`:

```text
microos> ls /bin/ports
basename
cksum
cmp
command
dirname
echo
false
kilo
printenv
pwd
sha256sum
sleep
tee
true
ttyprobe
whoami
microos> kilo /notes.txt
```

The screen of the editor is the whole display. Type a line, then:

* `Ctrl-S` - save the file;
* `Ctrl-Q` - quit and come back to the shell.

The first picture of the editor takes its time: the port draws through the
framebuffer of the firmware and converts the cells that change, and a full
picture of 1920x1080 is a copy of 8 MB (`docs/ports/x86_64/uefi.md`). In the
emulator of this project that is tens of seconds; the console of the serial
port has none of that cost, and `Ctrl-S`/`Ctrl-Q` are read from the keyboard
as they are typed.

## 6. The network

The image of this page is built as the **plain machine**: the kernel, the
filesystems, the shell, the programs. It carries no network stack, and the
machine says so honestly:

```text
microos> netinfo
netinfo: there is no network device
microos> wget http://example.org/x /tmp/x
wget: command not found. Type 'help' for the list.
```

The wire of 2.0 - DHCP, the Intel NIC of a laptop, `wget`, `ping`, `telnet` -
is in the **complete machine**, the build of `make x86-all`, and a stick
written from that build is the same medium with the network on it (the
recipe is in "Build the image yourself"). A machine of the complete build
with a cable says, in the boot and in `netinfo` (these lines are the emulated
network of the project; a laptop's LAN gives its own numbers):

```text
e1000e0: 52:54:00:12:34:56, link up
net: 10.0.2.15/24, gateway 10.0.2.2, dns 10.0.2.3
dhcp: leased 10.0.2.15 for 86400 s from 10.0.2.2 (t1 43200, t2 75600)
...
microos> netinfo
netinfo: e1000e0, link up
  mac 52:54:00:12:34:56
  ip 10.0.2.15/24, gateway 10.0.2.2, dns 10.0.2.3
  dhcp: leased for 86400 s, 86400 s left (t1 43200, t2 75600), server 10.0.2.2
microos> ping 10.0.2.2
ping: 40 bytes from 10.0.2.2, 1 ms
```

The complete build also listens on TCP 23 with the shell of `telnet` and asks
for an account of `/etc/passwd`; `telnet off` closes it. The machine belongs
on the network of its owner and not on the open Internet
([SECURITY.md](../SECURITY.md)).

A network without a DHCP server leaves the machine with the address of the
emulator (`10.0.2.15`) and says `dhcp: no server, the fixed address of 1.8`:
the file of the machine is `/etc/network` (one `name value` a line: `dhcp 0`,
`address`, `netmask`, `gateway`, `dns`), and `kilo /etc/network` is the editor
that writes it. There is no Wi-Fi in this release: the card is the cable.

## 7. Two accounts, one console

The machine has the accounts of `/etc/passwd`: `root` and `guest`. The console
is root; `su - guest` becomes the other person (root becomes anybody without a
password) and `logout` comes back:

```text
microos> cat /etc/passwd
root:0:0:/:/bin/sh
guest:7:7:/home/guest:/bin/sh
microos> su - guest
microos> id
uid=7(guest) gid=7(guest) home=/home/guest shell=/bin/sh
microos> whoami
guest
microos> logout
logout: the session ends
microos> whoami
root
```

A file the guest writes is the guest's, and a file that root gives to a group
is read by the members of that group and not written by them
([ADR 0044](adr/0044-a-session-and-the-group.md), and
`tests/cases/test_users.c` is the matrix).

## 8. When something does not work

Read the console from the top. The table names the symptom, the line that says
what happened, and the move. "Send this" is what to put in a report: the whole
console of the boot, saved with a camera or a serial cable, is always the best
answer.

| Symptom | The line to look for | What to do |
|---|---|---|
| the firmware does not offer the stick | - | UEFI boot menu (`F12`/`F9`/`F11`); the entry with `UEFI:`; Secure Boot off |
| `Secure Boot` refusal, or the firmware says the application is not signed | - | turn Secure Boot off in the firmware setup; the loader is signed by the project, not by Microsoft |
| the screen stays black after the firmware logo | on a serial port: `uefi: the picture is ...`; without one, nothing | the firmware gave no picture this port can draw; read the lines over a serial port if the machine has one (115200 8N1) and send the `uefi:` lines of the boot |
| `root: the root is mfs on boot0p1` and `read-only` | `usb0: ... another system`, or no `usb0:` line at all | the stick's second partition was not taken for the root: the USB controller did not come up, or the image was written wrong. The machine works read-only; rewrite the stick |
| `write: /x.txt: input/output error` | `root: boot0p1 ... read-only` | the same: the root is the copy in the ESP. A write there is a write of the memory |
| typing does nothing | `ps2: the keyboard speaks set 2...` or `usb: keyboard ...` or neither | a laptop's keyboard is PS/2 through its embedded controller; an external one must be on a USB port. Neither line = the port did not find a keyboard: send the two `ps2:`/`usb:` lines of the boot |
| `shutdown` ends with `this machine has no software switch-off` | `acpi: the FADT ...` or `acpi: the firmware has no FADT` | press the power button; the data is on the stick already |
| a file is gone after the power | `root: the root is mfs on boot0p1` | the machine booted the image of the ESP (see above); the file was written to the memory |
| `date` names a year that is not yours | `date` | the CMOS battery of the machine, or a firmware whose century register is odd; `date set YYYY-MM-DD HH:MM:SS` writes the clock |
| `exec: kilo: not found in /bin:/bin/ports:/usr/bin` | - | the image predates 1.9; a release of 1.9 or later carries `/bin/ports` |
| `netinfo: there is no network device` | that line | this image has no network stack; the complete machine of `make x86-all` has it (section 6) |
| `netinfo: e1000e0, link down` | that line | the cable, or a port whose PHY does not come up; Wi-Fi is not supported |
| `dhcp: no server, the fixed address of 1.8` | that line | the LAN has no DHCP server: `kilo /etc/network` with `dhcp 0` and the address of the machine |
| the boot stops at `usb: the port N is not enabled after its reset` | that line | a controller this port cannot finish; send the `usb:`/`xhci:` lines and the model of the laptop |

Two more moves that solve most of the rest:

* **the serial console**: a machine with a COM1 (a desktop, a dock, a firmware
  that redirects its console) shows the same log and accepts the same
  commands at 115200 8N1; a machine whose screen is dead and which has none is
  a machine that needs the display;
* **the same image in QEMU**: `make uefi-stick` then
  `python3 tools/uefi_stick_smoke.py --keep` boots the very bytes of the stick
  under OVMF, which is the emulated laptop of this project. If the image boots
  there and not on the laptop, the difference is the firmware of the laptop,
  and the console of the two is the comparison.

## 9. What this machine does not do yet

The honest list, with the place each item is tracked:

* **the touchpad and the mouse of the laptop** are not read; the keyboard is.
  ([`docs/debts.md`](debts.md))
* **Wi-Fi** is in neither build: the wire is the network of this release
  ([`docs/plan-wifi.md`](plan-wifi.md));
* **the network** is in the complete build and not in the image of this page
  (section 6), and its certificate store is the bundle of `/etc/ca` of that
  build ([`docs/wiki/network.md`](wiki/network.md));
* **the second partition is the root**: no installer adds a partition beside
  the files of a stick, and the image erases the stick it is written to
  ([ADR 0056](adr/0056-the-stick-of-a-person-is-a-disk.md), K12);
* **no suspend and no lid**: `shutdown` and `reboot` are the two ends
  ([ADR 0050](adr/0050-the-machine-that-switches-itself-off.md));
* the **board** is another machine: its page is
  [`docs/ports/esp32`](ports/esp32/README.md).

## 10. Build the image yourself

The same stick, from the sources, on a Linux machine with the tools of the
table above:

```bash
git clone <the repository of this tree>
cd Micro_OS
make uefi-stick          # build-uefi/stick.img, 90 MB
ls -l build-uefi/stick.img
```

`make uefi-stick` builds the kernel of x86-64, the programs of `/bin` and of
`/bin/ports`, the volume of 24 MB and the ESP, and writes the table of the two
partitions ([`docs/wiki/build.md`](wiki/build.md) has the packages and the
errors of the build). The file is the same medium the release carries: a
`make bundle` after `make uefi-stick` copies it into `dist/` as
`MicroOS-<version>-uefi.img` (`tools/bundle.sh`; without a stick built, the
bundle has no stick and the ISO alone).

The stick of the **complete** machine - the same medium with the network - is
the same tool and a build with the network on:

```bash
cmake -B build-x86-all -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMICRO_OS_X86_NET=ON -DMICRO_OS_TELNET=ON
cmake --build build-x86-all --target image uefi
python3 tools/x86_image.py --image build-x86-all/programs.img --build build-x86-all \
    --mkfs build-host/tools/microos_mkfs --ports build-ports/programs
python3 tools/uefi_stick.py --image build-uefi/stick-all.img --build build-x86-all \
    --mkfs build-host/tools/microos_mkfs --ports build-ports/programs
```

The gate of the medium proves the promise on the emulated machine: a file is
written, the power goes, the same bytes boot again and the file is read
(`make uefi-stick-smoke`, 21 checks). `make uefi-run` boots it in a window,
`make uefi-smoke` boots the ESP alone.

[Back to the README](../README.md) · [The wiki](wiki/README.md) ·
[What the machine does](wiki/getting-started.md)
