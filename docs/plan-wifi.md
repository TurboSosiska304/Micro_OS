# The WiFi of a laptop

> A reconnaissance, not a lane: whether the Intel Wireless-AC 8265 of a
> ThinkPad X1 Carbon (6th gen) can be put on the network of this system, what
> the licences allow, and what is cheaper than that. The decision is
> [ADR 0039](adr/0039-wifi-on-a-laptop.md); the open ends are rows of
> [debts.md](debts.md).

The question is not "can a driver be written" - a driver can always be
written - but "which driver, under which licence, at what price, and what does
the person get at the end of it". This page answers the three with the
sources of other people and the numbers of this tree, and it separates what
was read from what has to be measured on the machine.

## The answer in one paragraph

It can be done, and the only lawful path is the **BSD one**: OpenBSD's
`iwm(4)` drives this exact chip and is offered under a dual BSD-2-Clause /
GPL-2.0 licence (we take the BSD half), the 802.11 state machine it stands on
is the BSD `net80211`, and Intel's firmware is a redistributable binary file
that travels beside the driver and not in it. The Linux `iwlwifi` is not a
path at all: it is GPL-2.0-only, which ADR 0025 forbids on every link line of
the tree, and it is a **`mac80211` driver**, so a "shim" would be a port of
Linux's own 802.11 subsystem and not of a driver. The price of the BSD path is
**15-20 thousand lines** of ported C and **6-10 months of one person**, and it
begins only after the laptop boots from its own disk and has USB - therefore
it is **not a lane of 1.8**. Cheaper than WiFi on this machine is the **wire**:
the X1C6 carries an Intel I219-V at `00:1f.6` behind the Ethernet Extension
Adapter, and a permissive `e1000`-class driver is 2-3 thousand lines. The
three "cheap" radio alternatives (a USB-Ethernet adapter, a tethered phone,
the board as a radio) are all blocked by the same missing thing: this port has
no USB host of any kind.

## 1. The machine, as its firmware describes it

The model is the 20KH/20KG ThinkPad X1 Carbon 6th Gen (2018), and the facts
below come from Lenovo's platform specification, from the reports of owners
(ThinkWiki) and from the PCI database:

| What | Value |
|---|---|
| WiFi | Intel Dual Band Wireless-AC 8265, 2x2 802.11ac + Bluetooth 4.2 |
| its address | `02:00.0`, PCI `8086:24fd` (rev 78), "Wireless 8265 / 8275" |
| subsystem | `8086:0010` "Windstorm Peak" or `8086:1010` depending on the model |
| slot | M.2 2230, key A/E, PCIe x1 - one slot, shared with the WWAN option |
| wired | Intel Ethernet Connection (4) I219-V, `00:1f.6`, `8086:15d8` |
| the wire | **no RJ45 port**: the I219 is behind the ThinkPad Ethernet Extension Adapter Gen 2 |
| USB | 2x USB 3.1 type A, 2x Thunderbolt 3 (USB-C) |
| storage | M.2 2280 NVMe (PCIe), no SATA |
| serial | none |
| firmware | UEFI, with the legacy (CSM) option of that generation |

Two of those rows are the whole difficulty of the port and not of the driver:

* **the WiFi is at bus 2**, behind a PCIe root port. `ports/port_x86_64/src/pci.c`
  walks `bus < 1`: it finds the virtio card of QEMU because QEMU puts it on
  bus 0, and it would never see `02:00.0`. A bridge walk (or ECAM) is a
  prerequisite of the WiFi of this machine; the wired controller of it sits on
  bus 0 and does not need one;
* **the machine has no disk this kernel can read.** The only block device of
  the port is the IDE channel in PIO (`src/ata.c`) and the volume carried by
  the boot module; a laptop has NVMe, and the port has no NVMe, no AHCI and no
  USB. A laptop can boot the ISO (BIOS, read-only root in RAM) and cannot keep
  a file after the reset.

The firmware of the card is the second fact that shapes the work:

| What | Value |
|---|---|
| the file | `iwlwifi-8265-36.ucode`, **2,432,528 bytes** (`36.c8e8e144.0`) |
| API versions | the driver accepts **22 to 36**; 36 is the newest published |
| the name in OpenBSD | `iwm-8265-36` (the same bytes, the BSD name) |
| where it must live | a file of the volume (`/lib/firmware/`), read by the driver through the VFS |
| does it fit the image | no: the x86-64 volume is 16 MB with ~400 KiB free (debts, K6) |

The card is not in this tree's machines: QEMU's PC has no 8265, so every
level below is measured on the laptop or not at all. That is the reason the
plan puts the work in steps with a gate each: a step that cannot be proved on
the machine is not done.

## 2. Why the Linux driver is not a path

`drivers/net/wireless/intel/iwlwifi/` is the reference implementation of this
device and it is out of reach twice.

**The licence.** Its files are `SPDX-License-Identifier: GPL-2.0-only` (the
`Kconfig` of the directory says so on the first line), and ADR 0025 forbids
GPL code on any link line of a binary this tree builds. The user-space form of
rule 2 does not rescue it either: a program of this system gets no PCI
configuration space, no physical address for a DMA ring and no interrupt, and
the ABI has no such call (`kernel/ksys.h`). A driver is not a program of ring
3 in this system, so a copyleft driver has no home here at all.

**The shape.** `iwlwifi` is a `mac80211` driver: its `Kconfig` says
`depends on PCI && HAS_IOMEM && CFG80211`, and its `mvm/ops.c` calls
`ieee80211_alloc_hw()`. The 802.11 state machine - the scan policy, the
authentication and association, the block-ack agreement, the rate control,
the key handshake - is **not in the driver**: it is in `net/mac80211/` and
`net/wireless/` of the Linux kernel, well over a hundred thousand lines that
lean on `sk_buff`, `cfg80211`, workqueues, RCU and a netlink ABI. A "compatibility
shim" for `iwlwifi` is therefore a port of half of Linux's network stack, and
the day it compiles the licence question has not moved.

That is worth writing down because the intuitive plan - "take the vendor
driver and wrap it" - is the expensive one, not the cheap one.

## 3. The BSD path

The BSDs solved this device once already, under a permissive licence, and
their split is the one this kernel can live with.

### 3.1 `iwm(4)`, the driver of the device

| What | Value |
|---|---|
| files | `sys/dev/pci/if_iwm.c` (12,243 lines), `if_iwmreg.h` (6,842), `if_iwmvar.h` (693) |
| licence | dual **BSD-2-Clause / GPL-2.0** (Intel), plus Damien Bergamini's ISC - we take BSD |
| the device | `PCI_PRODUCT_INTEL_WL_8265_1` = `0x24fd`, firmware `iwm-8265-36` |
| also supported | 3160, 3165, 3168, 7260, 7265, 8260, 8265, 9260, 9560 |
| FreeBSD | `sys/dev/iwm/if_iwm.c`, 7,219 lines, imported from OpenBSD (older revision), same licence |

The file is the same driver in both trees; OpenBSD's is the living one (the
revision of 2025 adds the 9000 series and a decade of fixes), and FreeBSD's is
the proof that the file ports to a second kernel with a different `mbuf` and a
different `bus_dma`.

What it does **itself** - the whole conversation with the firmware, which is
where the difficulty of this chip lives:

* the firmware image: the TLV sections, DRAM/SSM/SMEM, **paging** (4 KiB
  pages, a paging image that can be megabytes), the alive handshake;
* the NVM (the per-card calibration and the MAC address), the PHY database,
  the regulatory data the firmware carries;
* the transport: 256-entry TX and RX rings, the command queue, the firmware's
  own answer queue, the `TFD`/`RB` descriptors and their physical addresses;
* the MAC and PHY contexts, the station context, the binding command, the
  EDCA and slot/DTIM updates, the power command and the beacon filter;
* **scan**: the LMAC/UMAC scan command per channel, the probe request the
  driver builds, the dwell time, the abort;
* **authentication and association**: the frames are sent by the firmware
  (`iwm_auth`), the answers come back as notifications;
* **the keys**: `IW_ADD_STA_KEY`, with `IWM_STA_KEY_FLG_CCM` - the device does
  CCMP in hardware; the driver only reads the packet number of a decrypted
  frame (`iwm_ccmp_decap`, no AES in the driver at all);
* aggregation and block-ack, the rate table of a station, the temperature
  throttling, the LED, the interrupt handler (MSI-X, or MSI with a chicken
  bit forced - `IWM_UREG_CHICK_MSI_ENABLE`), the error dump.

What it **delegates** to `net80211` - 89 distinct symbols, and this is the
list that decides the size of the port:

* the node table (`ieee80211_node`, 65 uses), the BSS/ESS bookkeeping, the
  channel list and the `ieee80211com` state;
* the state machine (`ieee80211_new_state`, `ieee80211_begin_scan`), the scan
  policy, the probe request/response handling, the IE parser;
* the **WPA2 supplicant**: `ieee80211_pae_input.c` and
  `ieee80211_pae_output.c` implement the four-way handshake, the PBKDF2 of the
  passphrase, the PRF of the PTK and the AES key unwrap of the GTK;
* CCMP as a software fallback (`ieee80211_crypto_ccmp.c`), the rate control
  (`ieee80211_ra.c`, AMRR), the regulatory domain, the HT/VHT negotiation.

### 3.2 `net80211`, the 802.11 stack

| What | OpenBSD | FreeBSD |
|---|---|---|
| files | 31 | 84 |
| bytes | 725 KiB | 2.0 MiB |
| the station core (12-17 files) | **~17.7 thousand lines** | ~20 thousand lines |
| licence | BSD-2/BSD-3 (Onoe, Leffler and the NetBSD line) | BSD-2 |
| WPA2 supplicant | in the kernel (`ieee80211_pae_*`) | hostap in user space |

OpenBSD's is the one to take, for the same reason `iwm` is: they are written
for each other. Its station core is what a client needs; the rest of the
directory - mesh, hostap, WDS, TDMA, DFS, the monitor mode with radiotap, the
ALQ logging - is not ported, and none of it is on a path to an associated
station.

The dependencies of those files on their own kernel are narrower than the
size suggests:

| They want | Uses | We have | Gap |
|---|---|---|---|
| `mbuf` chains | 10 functions (`m_freem`, `m_adj`, `m_pullup`, `m_copydata`, ...) | flat frames of 1518 bytes | an `mbuf` shim or a rewrite of the I/O edges, ~400-600 lines |
| `ifnet` and `if_ioctl` | the interface, the media, the ioctls | `port_x86_64_net.h`, six calls | a small `ifnet` shim: name, MAC, up/down, a queue |
| `taskqueue`, `timeout` | 18 uses in `iwm` | kernel tasks, `k_sched_sleep_ms`, the port clock | a `task`/`timeout` shim over the existing task API |
| `bus_dma`, `bus_space` | 61 uses in `iwm` | `port_x86_64_bus_phys`, `port_x86_64_paging_map_device` | direct calls; no tag/map layer is needed on an identity map |
| `crypto/` (SHA-1, MD5, HMAC, key wrap) | the PAE and CCMP | mbedTLS 3.6.2 in the kernel image: AES, CCM, SHA-1, MD | PBKDF2 and RFC 3394 key wrap: ~150 lines, or two config defines |
| `sysctl`, `bpf`, `domains`, `vlan` | the edges | none | dropped, it is not on the station path |

That table is the honest estimate of the "glue": **1.5-2.5 thousand lines** of
shim, and the shims are where a port of this size usually dies, because they
are written against a kernel that is not ours.

### 3.3 The seam is already in our tree

The x86-64 port has exactly the seam a WiFi driver needs:
`ports/port_x86_64/include/port_x86_64_net.h` promises a name, a presence, a
MAC, a link, a receive and a send, and `ports/port_x86_64/src/virtio_net.c` is
one file behind it, and its own comment says as much: a second driver for
this machine is another file behind the same calls.

That is the strongest fact in this document: a WiFi driver ends its receive
path in `port_x86_64_nic_receive()` with an Ethernet frame (the 802.11 data
frame after the LLC/SNAP translation is an Ethernet frame), and its transmit
path starts in `port_x86_64_nic_send()`. Everything above - ARP, IPv4, ICMP,
UDP, TCP, the resolver with its cache, TLS, HTTP, MQTT, telnet, `pkg` - is
untouched. The 1,986 lines of `src/net.c` and the 7,515 lines of
`components/net` do not know whether the frames come from virtio or from the
air.

### 3.4 What the port itself is missing

Four things, each of them a debt row of its own, and none of them about
802.11:

1. **a walk of the PCI buses behind the bridges** - `pci.c` stops at bus 0;
2. **MSI/MSI-X and a vector a driver can own** - the port has bodies for the
   timer, the keyboard and the IPI, and `interrupts.S` names no other. The
   WiFi wants MSI-X (one vector) or MSI;
3. **a DHCP client** - the stack of the port is `10.0.2.15/24` with the
   gateway of QEMU's user network, and a real LAN hands out the address over
   DHCP;
4. **a place for a firmware file** - a volume with 2.4 MB free and a rule that
   says where a binary of a device lives and how it is licensed.

## 4. The five steps

The steps are the levels of the question "what does the person get", and each
one has a gate that can fail on the machine. The numbers of lines are the
ported/adapted code of the step, not the total.

| Step | What it is | Lines | Weeks | What the person gets | The gate |
|---|---|---|---|---|---|
| A | the card answers: PCI, MMIO, MSI, rings, firmware, alive, NVM | 2,000-3,000 | 6-10 | `wifi0: 8265, MAC ..., rfkill off`, in `netinfo` | the card is named, the firmware runs, the MAC is the one on the label |
| B | scan | 3,000-4,000 | 5-8 | `wifi scan` lists the networks around, with the signal | ten networks with their names, channels and RSN flags |
| C | association with an open network, and data | 3,000-4,000 | 5-7 | an open network works: `ping`, `http get`, DHCP address | a boot joins an open AP, `ping` the gateway, a page fetches |
| D | WPA2-PSK | 2,000-2,500 | 4-6 | the home network works | a WPA2 AP with a wrong key is refused, with the right one a page fetches |
| E | the world: rate control, A-MPDU/BA, power save, 5 GHz, roaming, regulatory | 5,000-8,000 | 10-20 | it works not only at the desk | a 100 MB download at the rate of the link, a walk out of range and back |

The totals are **15-20 thousand lines** and **6-10 months of one person** who
knows both kernels, plus the time the hardware takes. Most of it is ported
(BSD files, kept with their notices); the shims and the port's own missing
pieces are the smaller part and the only part written here for the first time.

Why the steps are in this order and not another: a scan without association
proves the firmware and the receive path, association without WPA2 proves the
data path, and WPA2 last because it is the only step that can be tested
offline - a wrong PTK is a wrong PTK whether or not there is an access point
nearby.

The first user-visible result is the end of step C, about **four to six
months** in. A person who wants the shell on the laptop over the air pays
those months before the first page; a person who wants a package install pays
them plus the DHCP client.

## 5. The alternatives, by price per result

The comparison is on this machine (the X1C6) and this tree, not on a desktop
with an RJ45 port.

| Path | What it needs | Lines | Time | What the person gets | Verdict |
|---|---|---|---|---|---|
| **the wire**: I219-V + the Gen 2 adapter | a `e1000`-class driver (OpenBSD `em(4)`, FreeBSD `e1000`, both BSD), bus 0, MSI or the legacy line | 2,000-3,000 | 3-6 weeks | a real LAN, DHCP, `pkg`, `wget`, `httpd` - everything but mobility | **the cheapest network on this machine**; the first lane |
| USB-Ethernet adapter | an xHCI host controller, a USB core, a class driver (CDC-ECM or the vendor's ASIX) | 8,000-14,000 | 4-8 months | the same as the wire, with a dongle | not cheaper than WiFi; and the USB core is a project of its own |
| phone tethering (RNDIS) | the same USB host, RNDIS, DHCP | 9,000-15,000 | 4-8 months | a network wherever the phone has one | the same as above plus a protocol nobody loves |
| the board as a radio | a USB host (the board's port is a CH9102 UART; the laptop has no serial), then a frame protocol over the serial line | 1,000-2,000 after USB | weeks after USB | ~75 KB/s at 921600 baud, twice that at 2 Mbaud: a shell and a small download | the cheapest *radio*, and still blocked by USB |
| an M.2 card with a BSD driver | an AR9280 is mini-PCIe, not M.2 2230; the M.2 Atheros are QCA6174/ath10k, which needs `mac80211`; an adapter, antennas, and the BIOS whitelist of the model | - | - | - | changes the firmware story, not the work: `net80211` stays |
| **WiFi on the 8265** | `iwm` + `net80211` + the firmware file | 15,000-20,000 | 6-10 months | the machine on the air, no dongle, no wire | the only real WiFi; after the machine boots |

The lesson of the table is uncomfortable and worth saying plainly: on a laptop
without a USB host, **WiFi is the only wireless path and the wire is the only
cheap path**, and the two share their first months (the DMA, the interrupt,
the DHCP client) with the disk the machine needs anyway.

## 6. What we reuse, and what we do not have

| Ours | What the WiFi uses it for |
|---|---|
| `kmm`, `k_port_alloc` | the rings, the firmware buffer, the node table |
| tasks, `k_mutex`, the port clock | the scan, the watchdog, the association, the reconnect |
| `port_x86_64_bus_phys` | the physical address of every descriptor (ADR 0032) |
| `port_x86_64_paging_map_device` | the 2 MiB window over the BAR of the card |
| `port_x86_64_pci_*` | the identifiers, the command register, the BAR, the size |
| `port_x86_64_net.h` | the seam: a frame in, a frame out |
| `src/net.c`, `components/net` | ARP, IP, ICMP, UDP, TCP, DNS, TLS, HTTP, MQTT, telnet, `pkg` |
| mbedTLS (AES, CCM, SHA-1, MD) | the PTK derivation and the key unwrap of WPA2 |
| the VFS | the firmware file and `/etc/wifi.conf` |
| the log, the shell, `netinfo` | the console of the driver |

| Not ours, and not in the plan's first three steps | Why it matters |
|---|---|
| 802.11 (frames, IEs, the state machine) | `net80211` is the port |
| WPA2-PSK | the PAE of `net80211`, or ~1,500 lines of our own |
| `cfg80211`, `nl80211`, `mac80211` | the Linux shape: not ported, not wanted |
| DHCP | the port has none; ~300 lines |
| regulatory domain, power save, roaming | step E; the firmware's own tables carry most of it |
| a firmware loader | 60 lines of VFS read, but it needs a volume to read from |

## 7. The order of the work

The WiFi is the last thing in a chain, and the chain is not a wish list: each
link is a thing the laptop cannot do today.

0. **the laptop boots and keeps a file.** UEFI or legacy, a disk (NVMe or the
   SD reader), a clock, a volume with room for 2.4 MB of firmware. Today the
   port boots the ISO into a read-only volume, has no NVMe, no AHCI, no USB
   and no CMOS clock; the ISO is not even a hybrid image, so a stick does not
   boot (`docs/debts.md`, K13).
1. **the wire.** The I219-V at `00:1f.6`: the PCI function is on bus 0, the
   driver is permissive, there is no firmware and no 802.11. It gives the
   machine a network, `pkg` and a way to copy the firmware file onto it. It
   also pays for the PCI walk and the interrupt work that the WiFi needs.
2. **USB.** It unlocks three of the four alternatives at once (a USB-Ethernet
   adapter, a tethered phone, the board as a radio) and it is how a machine
   with no serial port is debugged.
3. **WiFi, steps A to E.**

## 8. What would change the verdict

A plan that cannot be falsified is a wish. These are the things that would
make the answer "no" or "later" instead of "yes, after the disk":

* **the firmware does not run without a Linux/ACPI service the driver
  assumes** (the LTR of the PCIe link, the `SHADOW_REG` handshake). Then step A
  grows a chapter about the platform and not about 802.11. *To measure on the
  machine.*
* **the BIOS whitelist refuses a card that is not the one it shipped with**,
  which would close the "buy an Atheros" alternative for good. *To measure
  with a borrowed card.*
* **the board as a radio over USB gives under 100 KB/s for the shell and a
  package install**, which is what a 921600-baud line allows. Then the WiFi is a
  luxury for this machine and the honest lane is the wire plus the board.
* **the wire works and nobody misses the air**, which is the most likely
  outcome of the first two lanes. The WiFi then waits for the laptop to be a
  machine somebody carries, not a machine somebody ports to.

## Sources, and what was not verified

Read for this page (23 November 2026):

* `drivers/net/wireless/intel/iwlwifi/` of Linux: `Kconfig`, `cfg/8000.c`
  (`IWL8265_UCODE_API_MIN/MAX` 22/36), `mvm/ops.c` (`ieee80211_alloc_hw`),
  `pcie/drv.c` (`0x24fd`);
* `linux-firmware`: `WHENCE` and `LICENCE.iwlwifi_firmware` (the text of the
  redistribution and the "no reverse engineering" clause), and the size of
  `intel/iwlwifi/iwlwifi-8265-36.ucode`;
* OpenBSD `sys/dev/pci/if_iwm.c` (rev 1.419, 2025), `if_iwmreg.h`,
  `if_iwmvar.h`, and `sys/net80211/` (31 files);
* FreeBSD `sys/dev/iwm/` (the older import) and `sys/net80211/` (84 files);
* `wpa_supplicant`'s README: "licensed under the BSD license (the one with
  advertisement clause removed)", i.e. BSD-3-Clause;
* Lenovo's platform specification of the X1C6 and the ThinkWiki page of the
  model (the bus numbers, the I219-V, the M.2 slot);
* the PCI ID database (`8086:24fd`, `8086:15d8`).

Not verified, and marked as such wherever it matters: the BAR size of the
8265 (the driver maps what the BAR reports), the exact revision of the
firmware that the card in *this* laptop needs (the API range is 22-36; the
`dmesg` of the machine names the file the kernel asked for), and every number
of a step that is not measured on the machine. A port that has not booted on
the laptop has not started.
