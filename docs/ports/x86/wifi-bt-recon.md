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
* **HCI commands used by the M1 handshake** (Bluetooth Core Spec, Vol 4 Part E):
  * `HCI_Reset`, opcode `0x0C03` (OGF 0x03, OCF 0x0003).
  * `HCI_Read_Local_Version_Information`, opcode `0x1001`.
  * `HCI_Read_BD_ADDR`, opcode `0x1009`.
  Events come back as `Command Complete` (`0x0E`) with the opcode and return
  parameters.

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

## What this pass implements

* xHCI: interrupt IN transfer API and BT class attach/detach plumbing.
* `ports/port_x86_64/src/usb_bt.c`: HCI Reset + Read Local Version + Read
  BD_ADDR over USB HCI transport, no firmware load, no state change beyond
  reset.
* `ports/port_x86_64/src/bt_hci.c`: pure HCI response parsers, host-tested
  with synthetic Command Complete events.
* `bt` shell command: `bt info` prints module state and handshake identity.
* Without hardware (QEMU), the BT module registers as `KMOD_SKIP` with a clear
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
  Part E (HCI command/event formats: Reset 0x0C03, Read Local Version 0x1001,
  Read BD_ADDR 0x1009, Command Complete 0x0E). Normative spec; not copied into
  repo.

