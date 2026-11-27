# AMD VRAM surface and `present` (Polaris / DCE 1.1)

Status: implemented for a **linear RGB565** primary plane. The hardware format
is read on every `bufmap`, `bufflip` and `amdflip`; it is not inferred from the
framebuffer size. QEMU has no AMD display controller, so the hardware half of
this checklist is still an owner test on the desktop.

## What the commands show

```text
gpu vrammap
gpu bufalloc 4096 --name wm
gpu buflist
gpu bufmap 1
gpu bufmap 1 --yes
gpu bufflip 1
gpu bufflip 1 --yes --keep
gpu buflist
gpu bufunmap 1 --yes
gpu buffree 1
```

`bufmap` maps the allocation at the fixed kernel VA
`0xffffffff83d00000` plus the allocation's offset inside its first 2 MiB page.
It prints the allocation id/name, VA, allocation size, surface size, stride in
pixels and bytes, `GRPH_CONTROL` depth/format/bpp, `UNP_GRPH_CONTROL` tiling and
whether `present` is armed. `buflist` adds `mapped=...` and `active=...` columns.
For a 1280x800 RGB565 plane the surface is 2048 KiB and the stride is 1280
pixels / 2560 bytes; the VA and allocation size come from the live allocation.

Without `--yes`, `bufmap` is mapping-only and `present` keeps the RAM copy.
`gpu bufmap <id> --yes` is the explicit opt-in for later GRPH writes by
`k_blit_present`. Manual `bufflip`/`amdflip` still require `--yes` for every
write sequence. Restore writes are rollback writes and are always attempted
when a confirmed sequence or an active surface has to be withdrawn.

## Format and safety rules

* `GRPH_PITCH` is pixels per line; `GRPH_Y_END + 1` is the visible height.
* `GRPH_CONTROL.GRPH_DEPTH/GRPH_FORMAT` names the encoding. RGB565 is depth 1,
  format 1, two bytes per pixel. Depth 2 / format 0 is the 32-bit
  ARGB8888/ABGR8888 family, depth 2 / format 1 is ARGB2101010, and depth 3 /
  format 0 is the 64-bit floating-point family. Unknown encodings have no bpp.
* `UNP_GRPH_CONTROL` must report a linear array mode and no micro-tiling. A
  tiled or unreadable plane is refused for CPU drawing and page flipping.
* A non-RGB565 plane is reported honestly and `k_blit_present` leaves the
  software copy in use. `bufmap` refuses to describe such memory as an RGB565
  `k_fb_surface`.
* Allocations start above the first 16 MiB and exclude every enabled scanout
  region reported by the allocator. A mapped or active id cannot be freed.
* Only GRPH primary/secondary surface address and the unchanged pitch register
  are written. Timings, PLL, DDI, format, tiling and enable state are untouched.
* A log marker and `k_logfile_flush()` precede every GRPH write sequence. WC
  stores are fenced before scanout is pointed at the surface.
* Flip success requires a vblank edge and address/pitch/control/tiling
  readback. The original plane is restored by default. A failed rollback is a
  hard error, not a hidden software copy.

## `k_blit_present` selection

1. The source is the exact mapped RGB565 `k_fb_surface`, the shape and stride
   match, the plane is still compatible, and the surface was armed with
   `bufmap --yes`: the AMD backend points GRPH at the existing VRAM block. No
   RAM staging or pixel copy is made.
2. The source is RAM, the surface is not armed, or the plane is incompatible:
   the backend restores any active VRAM plane and answers `KERR_NOTSUP`.
   `k_blit_present` performs its normal software copy into the original
   framebuffer.
3. A flip or rollback fails: a driver error is propagated. The portable blit
   layer does not hide it with a copy into a destination that may not be live.

The refusal latch in `k_blit_present` is per source, so a RAM refusal does not
disable a later VRAM-surface flip.

## Owner checklist on the AMD desktop

1. Boot with no `amdgpu`/other display driver competing for the card. Keep a
   serial log and a known way to power-cycle if readback or rollback fails.
2. Record `gpu vrammap`: card/BARs, primary pipe, address, pitch, height,
   `format`, `bpp`, `control`, `tiling`, `linear`.
3. Allocate a buffer at least as large as the reported frame, then record
   `gpu buflist` and `gpu bufmap <id>`. The printed format must be `RGB565` and
   `linear=yes`; otherwise stop and keep the RAM path.
4. Run `gpu bufflip <id>` without `--yes`. It is a dry-run and must name the
   same plane, frame size and BAR0 target without writing GRPH.
5. Run `gpu bufflip <id> --yes` first. This flips and restores in one command;
   the log must end with `restored original surface`.
6. Run `gpu bufflip <id> --yes --keep`, then `gpu buflist`; `active=yes` is
   expected. Drawing tests may now use the mapped kernel surface. Run
   `gpu bufunmap <id> --yes`; it must restore and unmap before `gpu buffree`.
7. If any line says `WARNING restore not confirmed`, stop using the surface
   and power-cycle rather than freeing or remapping the block.
8. For the automatic path, run `gpu bufmap <id> --yes` and exercise
   `FBIO_PRESENT`/`k_blit_present`. A RAM source must still copy; the exact
   mapped source must return 0 without a staging allocation or pixel copy.

## Still needed before `wm` draws directly into VRAM

* A user-space mapping ABI that carries the VRAM buffer identity/offset into
  `FBIO_PRESENT`; the fixed VA in this slice is deliberately kernel-only.
* `wm` selecting that mapping as its canvas instead of its RAM backbuffer.
* A measured 32-bit-to-RGB565 path if the owner's plane reports
  `ARGB8888/ABGR8888`; the current safe answer there remains the RAM copy.
* More than one persistent mapped buffer, ownership/lifetime handling across
  process exit, and hardware validation of alternate modes.

## Provenance

Register indices/masks and the depth/format/tiling encoding are hardware facts
from Linux v6.12 `dce_11_0_d.h`, `dce_11_0_sh_mask.h`,
`drivers/gpu/drm/amd/display/dc/dce110/dce110_mem_input_v.c` and
`drivers/gpu/drm/amd/display/dc/dc_hw_types.h` (MIT). No amdgpu
code is copied; the mapping, state machine, rollback and tests are original
Micro OS work.
