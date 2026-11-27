# Intel 8265 WiFi — stage 2, virtual device and DMA

This slice proves the stage-1 loader path without the laptop: the DMA contract
is explicit, the loader allocates aligned DMA buffers, and a virtual 8265 runs
the whole start path to READY. No scan, association, WPA, TX/RX or radio
backend is added.

## Provenance

Register numbers, masks and protocol ordering are facts from Linux v6.12
`iwlwifi` (`iwl-csr.h`, `iwl-prph.h`, `iwl-fh.h`, `pcie/trans.c`,
`pcie/ctxt-info.c`, `iwl-context-info.h`, `fw/file.h`, GPL-2.0). No Linux
source is copied, linked or vendored; the implementation is original Micro OS
code under MIT and the relevant files carry the provenance note. Firmware files
remain data, not code; Intel's `LICENCE.iwlwifi_firmware` stays with the blob.

## What is proven by tests

* `k_port_dma_alloc(size, align)`/`k_port_dma_free(ptr)` is in the port
  contract (1.16): x86 returns 4 KiB-capable aligned identity-mapped heap
  memory, host allocates through `kmm` with alignment verification, ESP32/null
  provide implementations. Unit tests check alignment, size usability, free and
  repeated allocation.
* The loader uses `k_port_dma_alloc`/`k_port_dma_free` for RBD, used, status,
  chunk, bounce and context descriptor buffers. A bounce buffer covers firmware
  sections whose source address is not 4 KiB aligned.
* A virtual 8265 (tests only) models CSR/PRPH/FH MMIO, releases MAC clock on
  `MAC_ACCESS_REQ`, accepts DMA ucode, counts loaded bytes and CRC32, and posts
  an ALIVE notification. The whole path runs: attach/power-up state -> parse ->
  load -> ALIVE -> READY.
* Negative cases are caught: corrupted TLV, truncated firmware, missing ALIVE.
  Missing ALIVE ends by the loader timeout boundary and moves the state to
  FAILED with reason `ALIVE timeout`; it does not hang.
* The 8265 state machine is `DOWN -> POWER_UP -> FW_LOADING -> ALIVE -> READY`
  plus `FAILED`/`SKIP`. Each transition logs one line; `wifi info` prints state
  and failure reason.

## What remains for hardware

* Real PCIe DMA coherence and IOMMU/physical address behaviour on the X1 Carbon.
* Actual firmware acceptance by the 8265 and a real ALIVE notification.
* RF, NVM ownership, scan, association, WPA2/3 and data TX/RX (stage 3+).

## Laptop checklist

1. Install `iwlwifi-8265-36.ucode` and `LICENCE.iwlwifi_firmware` in
   `/etc/firmware`; boot with serial logging.
2. Run `wifi info`; confirm `8086:24fd`, sane `CSR_HW_REV`, phase `POWER_UP`.
3. Run `wifi start --yes`; expect state log lines to `ALIVE`/`READY`.
4. If it fails, capture the timeout/error line and `wifi info` reason; do not
   report success without an ALIVE line.
