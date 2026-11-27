# Intel 8265 WiFi — stage 1, the card answers

This is a development slice, not a release promise. The target is the Intel
Wireless-AC 8265 (`8086:24fd`) of the owner's ThinkPad X1 Carbon Gen 6. The
first milestone is deliberately small: find the PCI function, map BAR0, bring
up its clocks, parse and load the firmware file, and observe `ALIVE`. There is
no scan, association, DHCP, supplicant or frame transmission in this stage.

## Layers

| Layer | Files | Responsibility |
|---|---|---|
| Portable container | `components/wifi/include/wifi/iwl8265_fw.h`, `src/iwl8265_fw.c` | Bounded TLV parser; runtime/init/paging records and CPU separators |
| Firmware file | `iwl8265_fw_file.h`, `iwl8265_fw_file.c` | Find API 36 down to 22 in `/etc/firmware`, `/lib/firmware`, `/firmware`; read through VFS |
| Portable loader | `iwl8265_loader.h`, `iwl8265_loader.c` | Context descriptor, RX ring, chunk DMA, load status and bounded `ALIVE` wait behind an ops table |
| Register facts | `ports/port_x86_64/include/iwl8265_regs.h` | CSR/HBUS/FH offsets, `CSR_HW_REV` decode, injectable bounded-wait primitive |
| PCI/MMIO | `port_x86_64_wifi.h`, `src/wifi_8265.c` | PCI attach, D0, BAR0 map, interrupt mask, read-only identity registers, bounded power sequence |
| Module/shell | `ports/port_x86_64/src/wifi.c` | `kmod wifi`, `wifi info`, dry-run `wifi start`, explicit `wifi start --yes`, loader ops |

The kernel remains hardware-free. The ESP32-shaped station core in
`components/wifi/src/wifi.c` is unchanged in behaviour: an x86 read-only probe
does not install a radio backend, so `wifi_scan()` and `wifi_connect()` still
report `WIFI_NO_RADIO`.

## Provenance and firmware licence

Register numbers, masks and protocol ordering are facts from Linux v6.12
`iwlwifi`: `iwl-csr.h`, `iwl-prph.h`, `iwl-fh.h`, `iwl-io.c`, `pcie/trans.c`,
`pcie/rx.c`, `pcie/tx.c`, `pcie/ctxt-info.c`, `iwl-context-info.h`,
`iwl-drv.c`, `fw/file.h` and `cfg/8000.c`. Those files are GPL-2.0. No Linux
source is copied, linked or vendored; the implementation here is original
Micro OS code under MIT, and each relevant file carries the provenance note.

The firmware is **not source and is not embedded in the kernel**. Install an
unmodified `iwlwifi-8265-<api>.ucode` (API 22 through 36) as a data file in one
of the directories above. Redistribution is governed by Intel's
`LICENCE.iwlwifi_firmware`; keep that licence beside any distributed blob.
This slice never writes NVM/OTP and never issues the EEPROM read command.

## Safety and state

* Boot performs PCI attach and the bounded power handshake only. QEMU has no
  8265, so its result is `KMOD_SKIP` and no MMIO is touched.
* `wifi start` without `--yes` is a dry run. Firmware DMA starts only after
  `wifi start --yes`.
* Every register wait has a deadline and logs the register, last value and
  elapsed milliseconds on timeout.
* Device interrupts stay masked except the FH-TX completion during loading
  and ALIVE/RX while waiting for the notification. No MSI/MSI-X is enabled.
* `wifi info` prints PCI identity, CSR values, BAR0, parsed firmware API/build
  and the module/phase state. It sends no command and transmits no frame.
* A missing firmware file is `KMOD_SKIP`, with the installation path printed.

## Tests and gates

Host tests cover the TLV format and hostile lengths, API 22..36, CPU/paging
separators, `CSR_HW_REV` decode, bounded waits, power ordering/timeouts, and the
no-card `KMOD_SKIP` path. The production gates are:

```sh
make test
make integrity
make x86-build
make x86-smoke       # when no other qemu-system-x86 process is running
```

A real card is required for attach, DMA and `ALIVE`; QEMU cannot prove those.

## Owner's laptop checklist

1. Install `iwlwifi-8265-36.ucode` and `LICENCE.iwlwifi_firmware` on the Micro OS
   volume, then boot with serial/console logging enabled.
2. Run `wifi info`; confirm the `8086:24fd rev ..` line, non-`0xffffffff`
   `CSR_HW_REV`, and phase `powered`.
3. Run `wifi start` (dry run), then `wifi start --yes`; save the complete log.
4. Confirm either `ALIVE status 0xcafe` or a named timeout/error line; do not
   report the stage complete from PCI/CSR discovery alone.
5. Confirm `kmod wifi` and `wifi info` never start a scan or association, and
   that the card's MAC/NVM contents remain unchanged.
