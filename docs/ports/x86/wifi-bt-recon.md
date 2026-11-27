# x86-64 Wi-Fi / Bluetooth recon (ThinkPad X1 Carbon Gen 6, Intel 8265)

Status: **recon by sources only**. The owner's laptop was unavailable during
this pass, so nothing below was read from live hardware. Every fact is tied to
a source; anything that needs confirmation on the machine is marked as such.

## How to confirm on the laptop when it returns

Run these on the laptop (Linux):

```sh
lspci -nn | grep -i network          # Wi-Fi PCI device + [vendor:device]
lsusb | grep -i bluetooth             # BT USB device + [vendor:device]
dmesg | grep -i iwlwifi | head -n 30  # firmware file the kernel loads
dmesg | grep -i bluetooth | head -n 30
```

For a clean descriptor dump (BT endpoints), after identifying the BT bus/dev:

```sh
lsusb -v -d 8087:0a2b   # replace with the actual VID:PID
```

## Module (expected)

| Part | Expected device | Bus | Source |
|---|---|---|---|
| Wi-Fi | Intel Wireless 8265 / 8275, PCI `8086:24fd` | PCIe (Wi-Fi function) | pci-ids: <https://pci-ids.ucw.cz/read/PC/8086/24fd> ("Wireless 8265 / 8275"); Linux v6.12 `drivers/net/wireless/intel/iwlwifi/pcie/drv.c` lists `IWL_PCI_DEVICE(0x24FD, ...)` with `iwl8265_2ac_cfg` / `iwl8275_2ac_cfg`. |
| Bluetooth | Intel Bluetooth, USB `8087:0a2b` | USB (internal, on the same M.2 card) | Linux v6.12 `drivers/bluetooth/btusb.c` quirks table: `{ USB_DEVICE(0x8087, 0x0a2b), .driver_info = BTUSB_INTEL_COMBINED }`. |

The PCI-ID page for `8086:24fd` also shows common subsystems such as
`8086:0010/1010/1130/0150` ("Dual Band Wireless-AC 8265 ..."). The exact
subsystem on the X1 Carbon Gen 6 must be confirmed on the laptop.

Note: pci-ids has a separate entry for USB `8087:0a2b` in the USB ID space;
the live confirmation command above is authoritative for the machine at hand.

## Wi-Fi (PCIe) — what the sources say

* **PCI ID**: `8086:24fd`, name "Wireless 8265 / 8275" (pci-ids). Linux
  `iwlwifi/pcie/drv.c` maps many `24FD` subsystems to `iwl8265_2ac_cfg` or
  `iwl8275_2ac_cfg`.
* **Driver/config**: Linux v6.12 `drivers/net/wireless/intel/iwlwifi/cfg/8000.c`
  defines `iwl8265_2ac_cfg` with `fw_name_pre = "iwlwifi-8265"`,
  `IWL8265_UCODE_API_MAX = 36`, `IWL8265_UCODE_API_MIN = 22`, device family
  8000. The firmware file name pattern is `iwlwifi-8265-<api>.ucode`
  (`IWL8265_MODULE_FIRMWARE(api)`).
* **Firmware files** (linux-firmware WHENCE, iwlwifi section): `iwlwifi-8265-21.ucode`
  through `iwlwifi-8265-36.ucode` are present; WHENCE lists license as
  "Redistributable. See LICENCE.iwlwifi_firmware for details" and also points to
  <http://wireless.kernel.org/en/users/Drivers/iwlwifi#Firmware>. The firmware
  is Intel proprietary but redistributable under its own license — **not GPL**.
* **BARs/MMIO/DMA**: iwlwifi is a PCIe device driven through memory-mapped
  registers and DMA rings (transport layer in `iwlwifi/pcie/`). Exact BAR
  layout and register programming are defined by the Linux driver and the
  undocumented Intel firmware interface; reading the Linux source gives
  knowledge, but copying code would be GPL-tainted.

## Bluetooth (USB) — what the sources say

* **USB ID**: `8087:0a2b` is handled by Linux v6.12 `drivers/bluetooth/btusb.c`
  as `BTUSB_INTEL_COMBINED` (`{ USB_DEVICE(0x8087, 0x0a2b), .driver_info = BTUSB_INTEL_COMBINED }`).
* **USB class**: Bluetooth HCI over USB uses device/interface class
  `0xE0` (Wireless Controller), subclass `0x01` (RF Controller), protocol
  `0x01` (Bluetooth). `btusb.c` also matches the generic
  `{ USB_DEVICE_INFO(0xe0, 0x01, 0x01) }`. This is the USB Bluetooth HCI
  transport (Bluetooth Core Specification, Vol 4 Part B "USB Transport Layer").
* **Endpoints (standard HCI USB transport, to confirm on laptop)**:
  * EP0: control transfers. HCI commands are sent as class control requests
    (bmRequestType `0x20`, bRequest `0x00`, wValue `0x0000`, wIndex `0x0000`).
  * Interrupt IN: HCI events from controller to host.
  * Bulk OUT / Bulk IN: ACL data (not needed for the handshake slice).
  * Isochronous IN/OUT: SCO audio (not used here).
  The exact endpoint addresses/sizes/intervals for `8087:0a2b` must be read
  from the live descriptor dump (`lsusb -v -d 8087:0a2b`) when the laptop
  returns. The implementation must not hardcode "probably 0x81"; it reads the
  descriptors.
* **HCI surface implemented** (Bluetooth Core Spec, Vol 4 Part E):
  * `HCI_Reset`, opcode `0x0C03` (OGF 0x03, OCF 0x0003).
  * `HCI_Write_Local_Name`, opcode `0x0C13`, with the fixed 248-byte name
    parameter (`bt name <text>`).
  * `HCI_Inquiry`, opcode `0x0401`, and `HCI_Inquiry_Cancel`, opcode `0x0402`
    (`bt scan on|off`). The command is sent and its acceptance is parsed;
    inquiry results are not collected.
  * `HCI_Read_Local_Version_Information`, opcode `0x1001`.
  * `HCI_Read_BD_ADDR`, opcode `0x1009`.
  Events come back on interrupt IN. The parser handles the packets in the
  table below; a command waiter consumes unrelated valid events instead of
  mistaking the first packet for its answer.

| Event | Code | Parameter length | What this slice does |
|---|---:|---:|---|
| Disconnection Complete | `0x05` | 4 | Parses status, connection handle and reason; logs it. |
| Command Complete | `0x0E` | 3 + return parameters | Matches the opcode and reads the command status. |
| Command Status | `0x0F` | 4 | Parses status, command-packet credit and opcode. |
| Hardware Error | `0x10` | 1 | Parses the hardware code and marks the module failed. |
| Number Of Completed Packets | `0x13` | 1 + 4 × handles | Parses every handle/count pair and saturates ACL accounting at zero. |

## Firmware / licenses

| Component | Firmware | License | Notes |
|---|---|---|---|
| Wi-Fi 8265 | `iwlwifi-8265-*.ucode` (API 22-36 in linux-firmware WHENCE) | Redistributable, Intel license (`LICENCE.iwlwifi_firmware`) | Proprietary binary, redistributable; not GPL source. |
| BT 8265 (Intel) | Intel Bluetooth firmware/patches, typically `intel/ibt-12-16.sfi` + `.ddc` for 8265-class devices (linux-firmware) | Redistributable, Intel license (`LICENCE.ibt_firmware`) | Exact file for 8087:0a2b must be confirmed on laptop dmesg; do not guess. |
| Linux drivers | `iwlwifi`, `btusb`, `btintel` | GPL-2.0 | Knowledge can be used; code cannot be copied into Micro OS. |

## Three main obstacles for Wi-Fi

1. **Firmware loading protocol is complex and proprietary.** iwlwifi loads a
   large ucode image and follows a device-specific bring-up sequence (CSR
   registers, DMA queues, alive notifications, NVM/calibration). The firmware
   files are redistributable but not GPL; the load protocol is learned from
   GPL iwlwifi source and cannot be copied.
2. **A real 802.11 MAC/MLME is required.** Beyond PCIe transport, Wi-Fi needs
   scan/auth/assoc, 802.11 state machines, WPA2/WPA3 crypto, rate control, and
   regulatory handling. This is far larger than the BT HCI handshake slice.
3. **DMA/MMIO on real hardware needs careful mapping and IOMMU awareness.** The
   iwlwifi PCIe transport uses descriptor rings in DMA memory and device
   registers in BAR0; it assumes cache-coherent DMA and correct physical
   addressing. Our xHCI work gives us some DMA/physical-address discipline,
   but Wi-Fi brings a much larger, performance-sensitive DMA surface.

## What works, and what does not

The owner's laptop was unavailable for this pass, so **no live `bt info`
output was captured**. In QEMU, which has no Bluetooth controller, the
command registers and honestly reports:

```text
bt: state skip
bt: no Bluetooth controller found
bt: ACL data not tested: no controller
```

What the sources and synthetic tests establish:

* USB class attach, descriptor-driven interrupt/bulk endpoint setup and the
  Reset / Read Local Version / Read BD_ADDR handshake.
* `bt info`, `bt reset`, `bt name <text>` and `bt scan on|off` on x86-64.
  `scan` sends Inquiry or Inquiry Cancel and parses command acceptance only.
* Interrupt-IN parsing for Command Complete, Command Status, Number Of
  Completed Packets, Hardware Error and Disconnection Complete, including
  malformed/truncated packet rejection and an event-table unit test.
* Both bulk endpoints are configured when the descriptor provides them;
  `bt info` reports whether each opened. Number Of Completed Packets updates
  an outstanding counter without underflow. **No ACL packet was sent**, so
  the data path itself remains unverified.

The honest boundary is still a transport/control slice, not a Bluetooth
stack: there are no inquiry results, no ACL transmitter/receiver state
machine, no connection commands, no L2CAP, no ATT and no GATT. The first real
connection needs ACL framing and flow control over the bulk channels, the HCI
connection state machine, then L2CAP signaling/channel setup; GATT sits on
ATT above that and is a separate layer again.

## What this pass implements

* xHCI: interrupt IN transfer API and BT class attach/detach plumbing.
* `ports/port_x86_64/src/usb_bt.c`: the handshake, radio controls, event
  dispatch and ACL endpoint/accounting checks; no firmware load and no ACL
  data transfer.
* `ports/port_x86_64/src/bt_hci.c`: pure event-table and response parsers,
  host-tested with synthetic valid and malformed packets.
* `bt` shell command: status plus Reset, Write Local Name, Inquiry and Inquiry
  Cancel. Without hardware (QEMU), the module is `KMOD_SKIP` with a clear
  message.

## Sources (provenance)

* Linux v6.12 `drivers/bluetooth/btusb.c` — USB HCI driver, ID table including
  `8087:0a2b` as `BTUSB_INTEL_COMBINED`, generic `0xe0/0x01/0x01` match:
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/bluetooth/btusb.c>
* Linux v6.12 `drivers/net/wireless/intel/iwlwifi/cfg/8000.c` — 8265/8275 cfg,
  firmware name `iwlwifi-8265`, API 22-36:
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/cfg/8000.c>
* Linux v6.12 `drivers/net/wireless/intel/iwlwifi/pcie/drv.c` — PCI ID table,
  `0x24FD` subsystem mappings to 8265/8275 cfg:
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/net/wireless/intel/iwlwifi/pcie/drv.c>
* pci-ids `8086:24fd` — "Wireless 8265 / 8275":
  <https://pci-ids.ucw.cz/read/PC/8086/24fd>
* linux-firmware WHENCE (iwlwifi entries, license text "Redistributable. See
  LICENCE.iwlwifi_firmware for details"):
  <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/WHENCE>
  (fetched via raw mirror during recon; confirm on kernel.org when accessible)
* Bluetooth Core Specification 5.x, Vol 4 Part B (USB Transport Layer) and Vol 4
  Part E (HCI command/event formats: Reset 0x0C03, Write Local Name 0x0C13,
  Inquiry 0x0401, Inquiry Cancel 0x0402, Read Local Version 0x1001, Read BD_ADDR
  0x1009; Command Complete 0x0E, Command Status 0x0F, Number Of Completed
  Packets 0x13, Hardware Error 0x10, Disconnection Complete 0x05). Normative
  spec; not copied into repo.

