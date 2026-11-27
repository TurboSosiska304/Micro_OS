# AMD GPU write plan (Polaris / gmc_v8_0)

Status: plan only, no write code in this slice. The current probe is read-only
(PCI config, BAR5 MMIO reads, one read-only 4 KiB BAR0 aperture sample).

## Goal

Make a safe "write-read" test on a machine where Micro OS is the only driver,
without corrupting the live scanout.

## Preconditions

- Micro OS owns the GPU; no other driver (amdgpu, efifb, GOP handoff still
  active for scanout) is writing VRAM.
- Firmware/SMU is not required for the first test; use only BAR0/VRAM aperture
  reads and writes, no ring submission.
- The test runs only after the display path is either disabled or moved to a
  known buffer that we own.

## Finding a scanout-free region

1. **Read scanout registers (read-only first).** On DCE 11 / Polaris, CRTC
   base addresses are in `mmCRTC_CONTROL` / `mmCRTC_PRIMARY_SURFACE_ADDRESS`
   (DCE block). Read the primary/secondary surface addresses and pitch/size
   for enabled CRTCs. Facts to take from Linux DCE 11 headers with attribution.
2. **Read MC_FB_LOCATION / visible VRAM layout.** `mmMC_VM_FB_LOCATION` gives
   the FB aperture window; `mmCONFIG_MEMSIZE` gives real VRAM size. The VBIOS
   frame buffer is typically at the top or bottom of visible VRAM; Linux also
   reserves stolen/reserved regions reported by `amdgpu_vram_mgr`.
3. **Choose a probe offset outside scanout.** Pick an offset well above the
   largest possible scanout (e.g. 64 MiB + max mode size, aligned to 4 KiB)
   and below VRAM size. Confirm by reading CRTC base+size that the range
   `[offset, offset+4KiB)` does not overlap any enabled surface.
4. **Conservative fallback.** If scanout registers cannot be read confidently,
   do not write. Use the last 4 KiB of VRAM only after confirming no surface
   points there; prefer a dedicated "scratch" offset reported by firmware, but
   firmware is not loaded in this slice.

## Write-read test (when Micro OS is sole driver)

1. Map BAR0 page at chosen offset as uncached/WC as appropriate.
2. Save original 4 KiB contents to RAM.
3. Write a known pattern (e.g. 0x5A5AA5A5, address-tagged words).
4. Read back and compare; report pass/fail.
5. Restore original contents.
6. Unmap or keep mapping as needed.

## Risks

- Writing scanout VRAM corrupts the visible image on the owner's desktop.
- BAR0 may be WC-mapped; readback ordering needs a fence/barrier.
- Some offsets may be reserved by SMU/VBIOS; restoring is required.
- On a machine with active amdgpu, do not run the write test.

## What remains for "fill/copy on AMD"

- SDMA or compute ring for fills/copies (needs firmware load: SDMA ucode).
- GTT/TTM-like placement and domain management.
- VM/PT setup for GPU virtual addresses (GMC page tables).
- Fence/sync for command completion.
- Mode-setting ownership handoff before any scanout touching.
