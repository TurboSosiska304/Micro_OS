# 2.4 "Peaches": the system on its own disk

* Serov, *Girl with Peaches*, 1887 (`docs/release-names.md`).
* The light of the last minor: everything 2.3 left in pieces is put together.
* 2.3 shipped the installer that *plans*; 2.4 ships the one that *writes*, and a
  machine that boots from what it wrote.

## The one criterion

The release is done when this sentence is true and proven by a gate:

> the installer writes the system to a disk, the machine boots from that disk as
> a writable root, and `fsck` says the volume is consistent afterwards.

Everything else in this document is worth having, but none of it replaces that
sentence. If the sentence is not true, there is no 2.4.

## Lanes

A lane is done when its gate is green; "works on my NVMe" is not evidence.

### The install (the theme)

| Lane | Work | Gate |
|---|---|---|
| **I1** Format | `mkfs.ext2` and `mfs_format` on the partition the installer appended; `/etc/volumes`, `/etc/install`, the empty mount points | the volume mounts, `fsck`/`mfs_check` clean, the tree is there |
| **I2 Copy** | copy the system with a manifest: kernel, loader, `/bin`, `/etc`; SHA-256 of every file; sync; mount read-only and verify | a corrupted copy fails the check; QEMU install passes |
| **I3 Boot** | `\EFI\MICROOS\BOOTX64.EFI` in the ESP, the NVRAM question asked with no default answer, never touching a foreign boot file | OVMF boots the installed disk; the foreign ESP is byte-identical |
| **I4 Root** | boot the installed disk as a normal writable root, no regression of the live ISO/USB path | a file written after the first boot survives a reset |
| **I5 Gate** | `tools/x86_install_smoke.py`: 8 GiB image, a foreign partition with known bytes, install, boot, a smoke subset, hash of the untouched partition | green, and rerun by hand before the tag |

### The debts that make the install real

| Lane | Work | Gate |
|---|---|---|
| **D1 USB mass storage** | the stick must come up as `usb0` and be writable on metal (today the root falls back to the read-only boot module) | `usb-smoke` 66/66; on metal the log says `usb0: ... a volume of this system` and `/etc/run.log` is written |
| **D2 The page cache returns** | a background writeback that cannot race journal recovery | `cut-smoke` 105 and `uefi-stick-smoke` 21 on the same build; a file survives a power cut |
| **D3 The hang watchdog** | merge and use it in every gate: a timeout brings a dump, not "answered nothing" | `cut-smoke` red on purpose once, the dump names the place |
| **D4 Signal provenance** | who sent a signal, with `si_code`, for every delivery | `mouse-smoke` 7/7 and a `wm` that dies says who killed it |

### The metal

| Lane | Work | Gate |
|---|---|---|
| **M1 The other four cores** | the Ryzen brings up 8 of 12; the rest and the K8 debts (I/O APIC, wake IPI, FPU state, guard page) | `x86-smp-smoke` 76, five 3-minute soaks, `smp` reports every core of the machine |
| **M2 `#GP` 0x30 on AMD** | the return to ring 3 loads `SS` from the TSS high half on some path; the dump exists, the root cause does not | the desktop reaches `microos>`, runs `/bin/hello` and stays alive |
| **M3 AMD VRAM** | `vramtest --write` on real memory, the plane format read and not assumed (XRGB8888 + tiling on the desktop), `bufmap` + present without a copy | `wm` draws into a VRAM block on the RX 570, `bufflip` verified by `SURFACE_UPDATE_PENDING` |
| **M4 Intel GGTT** | 65 536 entries of 4 KiB; our decoded surface asks for 32 400 PTE, so the decode itself is suspect; `ggtt-map` on the laptop says who owns what | `ggtt-test` passes, or a proven statement that the device path needs no slot |
| **M5 WiFi** | the first real `ALIVE` on the laptop, then scan and association | `wifi info` shows `READY`; a scan lists the owner's networks |

## What is not in 2.4

* No encryption, LVM, swap, hibernate, automatic updates (3.0).
* No ARM port: the owner deferred it; the phone is a QEMU runner, not a target.
* No BIOS/MBR install: UEFI only, a foreign MBR is never touched.
* No modeset or backlight until an ADR of its own; the plan is written, the
  registers are not touched.
* No claim of `wm` speed until the device path is real: 570 µs a frame at
  1920×1080 is the software copy, not a flip.

## The rules that cost the most on the way to 2.3

These are not advice; they are the price already paid.

1. **One QEMU per machine.** A process older than twenty minutes with a dead
   parent is an orphan: kill it, do not wait for `host busy` to expire.
2. **Fewer lanes, more depth.** Two active lanes, the rest queued; six lanes on
   one machine is how a twenty-line bug costs six hours.
3. **Diagnostics are off by default.** Frame markers on COM1 flooded the desktop
   smoke; traces behind a flag, always.
4. **A gate that says "answered nothing" must bring a dump.** Without one it is
   a rumour, not a result.
5. **One source of truth per fact.** The site broke three times because the
   version, the painting and the feature list lived in three places.
6. **The lanes commit and push after every step.** A rate limit must cost
   minutes, not hours.
