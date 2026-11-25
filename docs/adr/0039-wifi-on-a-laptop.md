# ADR 0039 - WiFi on a laptop: the BSD path, and the order of the work

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0003 (the WiFi driver: a core and a radio), ADR 0004 (the
  portable boundary), ADR 0012 (many cores), ADR 0025 (copyleft outside the
  tree), ADR 0032 (the address of a buffer a device reads), ADR 0033 (a user
  and the console), `docs/licensing.md`, `docs/plan-wifi.md`, `docs/debts.md`

## Context

The machine of this question is a ThinkPad X1 Carbon 6th Gen: an Intel
Wireless-AC 8265 at `02:00.0` (`8086:24fd`), an I219-V at `00:1f.6` behind the
Ethernet Extension Adapter, NVMe storage, USB and no serial port. The tree's
x86-64 port runs in QEMU: one virtio-net card at bus 0, an IDE channel, a
read-only volume carried by the boot module, no bridge walk, no MSI, no USB,
no NVMe, no ACPI namespace and no clock that survives a reset. The
reconnaissance is [plan-wifi.md](../plan-wifi.md); this ADR is the decision it
asks for.

The obvious plan - "take the vendor driver" - dies twice. `iwlwifi` is
`GPL-2.0-only`, which ADR 0025 forbids on any link line of this tree, and it
is a `mac80211` driver: the 802.11 state machine lives in the Linux kernel,
not in the file, so even a lawful copy would be a port of half of Linux's
network stack. The user-space form of ADR 0025 does not rescue it either: a
program of this system is given no PCI function, no physical address for a
ring and no interrupt, and the ABI has no call that could give them.

The BSDs have the same device under a permissive licence, and their split is
the one this kernel can host: the driver of the device (`iwm(4)`, dual
BSD-2-Clause/GPL-2.0 - we take BSD) and the 802.11 stack (`net80211`,
BSD-2/BSD-3). The port is a port of those files, not a clean-room rewrite of
the firmware's command interface.

The x86-64 port also already has the seam the driver needs. ADR 0003 split the
WiFi of the board into a portable core (`components/wifi`: the state machine,
the configuration, the status) and a radio (`components/port_esp32`:
ESP-IDF). The x86-64 port has the same shape one layer down: a driver ends its
receive in `port_x86_64_nic_receive()` with an Ethernet frame and starts its
transmit in `port_x86_64_nic_send()`, and everything above - ARP, IP, TCP,
TLS, HTTP, `pkg` - does not know which controller answered.

## Decision

### 1. The driver of the 8265 is a port of `iwm(4)`, under its BSD half

`ports/port_x86_64/src/iwm.c` (with `if_iwmreg.h` in the include directory of
the port) is a port of OpenBSD's `if_iwm.c`, revision 1.419 or the newest that
supports the 8265. The file is dual-licensed and the tree takes the
**BSD-2-Clause** half, by a written choice, the way mbedTLS is taken as
Apache-2.0 (ADR 0025 §1). The copyright notices travel with the source, and a
row of `docs/licensing.md` names the file, the version and the licence the day
the port lands.

`iwlwifi` is not ported, not linked, not vendored and not read as code. Where
the behaviour of the firmware is unknown, the reference is the BSD file, which
Intel itself published under the permissive half; rule 3 of
`docs/licensing.md` - no code from the Linux kernel - is not bent by this ADR.

### 2. The 802.11 stack is `net80211`, a component of its own

`components/net80211/` is a port of OpenBSD's `sys/net80211/`, station side
only: the node table, the state machine, the scan policy, the IE parser, the
rate control, the regulatory domain, the PAE and the CCMP. Mesh, hostap, WDS,
TDMA, DFS, the monitor mode with radiotap and the ALQ logging are not ported.

It is portable C with a small `ifnet`/`mbuf` shim over the kernel this tree
has; it is not a part of `components/net`, which stays the L3 stack, and it is
not a part of the kernel core, which must not learn about 802.11.

The driver keeps the shape of ADR 0003: the portable core of the station
(`components/wifi`) sits above, the radio backend of the x86-64 port fills
`struct wifi_radio`, and `wifi_connect(ssid, pass)` reaches the PAE of
`net80211` through it. The state machine of the driver's core is not rewritten
a second time.

### 3. The firmware is a file, not source and not a program

`iwlwifi-8265-36.ucode` (2,432,528 bytes, API 22-36) is a binary of Intel
under `LICENCE.iwlwifi_firmware`: redistribution in binary form without
modification is permitted, the copyright and the disclaimer must travel with
it, and reverse engineering is forbidden. It is therefore:

* **never a file of this tree** - not in `components/`, not in `third_party/`,
  not in the image. A 2.4 MB blob is neither code nor a permissively licensed
  part, and the source tree is not where a device's firmware lives;
* **a file of the distribution**: `/lib/firmware/iwlwifi-8265-36.ucode` on the
  volume of the machine, and `LICENSE.iwlwifi` beside the bundle, the way
  `tools/bundle.sh` copies the licence of Spleen or mbedTLS. The driver reads
  it through the VFS; no new port contract is invented for it until a second
  device needs one;
* **not a package of `microos-progs`**: a package there is a `MOX1` image of a
  program, and a firmware is data. A checksum and the licence text in the
  release notes are what a person verifies;
* **installed by the person**, from the vendor or from a mirror of
  `linux-firmware`, exactly as OpenBSD and FreeBSD ship it (a firmware package
  of its own, outside the base system - the FreeBSD tree moved its iwlwifi
  blobs to ports in 2025 for the same reason).

The tree does not read the firmware, disassemble it or derive anything from
its bytes. The driver loads it and speaks to it.

### 4. The supplicant is the PAE of `net80211`, not `wpa_supplicant`

`wpa_supplicant` is BSD-3-Clause (the advertisement clause removed), so rule 1
of `docs/licensing.md` would let it into the tree. It is still not taken:

* it is a daemon of a POSIX user land that talks to a driver through netlink
  and `cfg80211` (or a per-driver ioctl), and this system has neither a socket
  call in the ABI nor a program that can own one (`kernel/ksys.h`, the next
  free number is 48);
* `net80211` already carries the client half - the four-way handshake, PBKDF2,
  the PRF of the PTK, the AES key unwrap of the GTK - in ~1,500 lines that are
  in the same licence and the same style as the stack it belongs to;
* two supplicants beside one driver is two definitions of the same state
  machine, and the second one would be tested by nobody.

The primitives come from mbedTLS, which is already in the kernel image of
x86-64 (AES, CCM, SHA-1, the message digest): PBKDF2-SHA1 and RFC 3394 key
unwrap are ~150 lines over `mbedtls_md_hmac`, or two defines of its
configuration. The device does CCMP in hardware (`IW_ADD_STA_KEY` with
`IWM_STA_KEY_FLG_CCM`); the software CCMP of `net80211` stays as the fallback
of a frame the firmware did not decrypt, and it is not on the WPA2 path.

### 5. WiFi is not a lane of 1.7 or 1.8, and the wire comes first

The order is a decision, not a preference:

1. **the laptop boots and keeps a file** - UEFI or legacy, a disk (NVMe or the
   SD reader), a clock, a volume with 2.4 MB free. A network on a machine that
   cannot write a file, keep a key or install a package buys nothing, and the
   ISO of today is not even a hybrid image that a stick boots (K13);
2. **the wire** - the I219-V at `00:1f.6`, an `e1000`-class driver under BSD,
   2-3 thousand lines, no firmware and no 802.11. It gives the machine a
   network and pays for the two things the WiFi needs as well: an interrupt
   that a device can own and the DMA path of a descriptor ring;
3. **USB** - it unlocks a USB-Ethernet adapter, a tethered phone and the board
   as a radio, and it is how a machine without a serial port is debugged;
4. **WiFi** - the five steps of `plan-wifi.md`: the card and its firmware, the
   scan, an open network, WPA2, and then the world.

The estimate of step 4 alone is 15-20 thousand lines and 6-10 months of one
person, with the first user-visible result (an open network, with an
address) after four to six months. That is a release of its own, not a lane
of a release that already has one. 1.7 and 1.8 do not carry it.

### 6. What the port gains before the first line of the driver

Each of these is a row of `docs/debts.md` today and a prerequisite of step A:

* a walk of the PCI buses behind the bridges (the WiFi of the laptop is at bus
  2; `pci.c` stops at bus 0) and, with it, the ECAM of a machine that has
  MCFG - the port's `0xCF8` window names any bus, but nothing walks the bridges
  that would find one;
* MSI/MSI-X and a vector that a driver can own, with the handler and the EOI;
  the port has bodies for the timer, the keyboard and the IPI only, and the
  8265 wants MSI-X (or MSI, with `IWM_UREG_CHICK_MSI_ENABLE`);
* a DHCP client, because the network of the port is `10.0.2.15/24` of QEMU's
  user network and a LAN hands out the address;
* a place for the firmware file and the rule of §3.

### 7. Nothing of this is in the code yet

This ADR decides the path, the licence and the order. It adds no file to
`components/`, no call to the port and no row to `docs/licensing.md`; those
come with the port, in the release that carries it. The plan, the debts and
this page are the record a person reads before starting, so that the first
commit of the driver is not also the first argument about its licence.

## Consequences

**Good**

* The WiFi of the machine most likely to run this system has a lawful path,
  and it is a port of files that were written for the same device, not a
  clean-room reverse engineering of a firmware interface.
* No GPL enters the tree: the permissive halves of `iwm` and `net80211` are
  ordinary components under rule 1, and the firmware is a file of the
  distribution under its own licence.
* The architecture does not move: the driver ends in the six calls of
  `port_x86_64_net.h`, ADR 0003's core/radio split is kept, and ARP, IP, TCP,
  TLS, HTTP and `pkg` do not learn that the frames came from the air.
* The order is honest about the machine: a wire and a disk before a radio.
  The interrupt and the DMA path paid for by the wire are the same ones the
  WiFi needs, and the DHCP client the wire adds is the one a WiFi LAN hands
  out the address with.

**Bad**

* The port is large: 15-20 thousand lines of adapted code and 6-10 months of
  one person, before the first release that carries it. A BSD file is not a
  BSD file on another kernel: the `mbuf`/`ifnet`/`task` shims are where the
  time goes, and they cannot be tested against the machine until the machine
  boots.
* The firmware is a 2.4 MB binary that the project distributes but cannot
  inspect, and it must not be reverse engineered. A firmware that misbehaves
  is a bug we report and work around, not one we fix.
* The laptop is a new machine for this port: the ISO is BIOS-only and not
  hybrid, the disk is NVMe, the clock is CMOS and the ACPI namespace is not
  read. The WiFi is the last of those problems, not the first.
* The estimate is an estimate. The plan's steps give the shape, and step A is
  the one that can fail for a reason nobody has written down yet.

**What the ADR does not do**

* It does not add `iwm`, `net80211` or a firmware to the tree; it fixes the
  licence and the shape they will have.
* It does not promise WPA3, 802.11w/PMF, monitor mode, an access point of our
  own, or 5 GHz/DFS: those are step E and later, and each has a price of its
  own.
* It does not make the board's WiFi and the laptop's one driver. The board is
  ESP-IDF behind the same core; the laptop is `iwm` behind the same core; the
  core is the only common part, which is ADR 0003 exactly.
* It does not decide the NVMe, AHCI, SD, USB or ACPI work that precedes it,
  and it does not make them smaller. It says they come first.
* It does not weaken rule 3 of `docs/licensing.md`. Reading a BSD file that
  Intel published under a permissive licence is not reading the Linux driver,
  and the difference is written down here so that a later contributor does not
  have to guess.

## Alternatives

**A. Port `iwlwifi` and its `mac80211`.** Rejected: GPL-2.0-only on a link
line (ADR 0025) and, independently, a port of a subsystem of two hundred
thousand lines instead of a driver of twelve.

**B. A clean-room driver of the 8265 from the firmware's interface.** Rejected
as the first attempt: the interface is undocumented outside the GPL and BSD
drivers, and the Intel licence forbids reverse engineering the blob. A
reimplementation would be a slower, later copy of `iwm` with a legal question
attached.

**C. `wpa_supplicant` as a program.** Rejected for now: it needs a socket ABI
and a driver interface of its own, and `net80211` already has the client half.

**D. A USB WiFi dongle with a BSD driver (`athn`, `urtwn`, `run`).** Rejected
as an alternative, not as a device: it needs a USB host, which is the same
prerequisite as the three cheap paths of `plan-wifi.md`, and it does not remove
`net80211`.

**E. Buy an M.2 card with an open driver.** Rejected as a way around the work:
the AR9280 has no firmware but is mini-PCIe and does not fit the slot; the
M.2 Atheros are `ath10k`, which is a `mac80211` driver, and the OpenBSD tree
does not support them. It would change the firmware story and not the stack.

**F. Do nothing and use the wire.** Not rejected. It is lane 2, and it is the
honest answer for a machine that is not carried out of the room.

## The gate of a later release

The ADR is revisited when a release plans the driver. That release opens with
three facts measured on the laptop and not in QEMU: the machine boots from its
own disk, the PCI walk finds `02:00.0`, and the firmware of the card is read
from the volume and answers alive. Until those three are green, the WiFi is a
plan and a debt, and this page says so.
