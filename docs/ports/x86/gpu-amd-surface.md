# AMD VRAM surface and `present` (Polaris / DCE 1.1)

Status: the code handles a **linear** primary plane in RGB565 and in 32-bit
XRGB8888; every VRAM write is behind `--yes`. No flip and no VRAM write have
yet been confirmed on the owner's RX 570. QEMU has no AMD display controller,
so the hardware half below is an owner test, not a green gate.

## What the live card told us (27 November 2026)

The host CachyOS `amdgpu` driver was running during this read-only measurement;
Micro OS did not touch the card. `modetest -M amdgpu -p` lists the primary
plane (id 45, CRTC 86) with `XR24 AR24 RA24 XR30 ... RG16 ...`; the CRTC uses
framebuffer 163 at 1920x1080@60. A `DRM_IOCTL_MODE_GETFB2` query on that id
returned `fourcc=XR24` (XRGB8888, 32 bpp), `1920x1080`, `pitch=8192`, modifier
`0x2000001d0d80400` (an AMD tiled modifier). So the desktop the owner boots is
**32-bit XR24 and tiled**, not RGB565 and not linear. The Micro OS register
values (`GRPH_CONTROL`, `GRPH_PITCH`, `UNP_GRPH_CONTROL`) are still read by
`gpu vrammap`/`gpu bufmap` on every call and are not guessed from this host
measurement.

Consequences: a `bufmap` on a tiled plane is refused honestly by the linear
check, and a 32-bit plane is refused when the kernel picture is RGB565. There
is no pixel conversion hidden behind `present`: the refusal carries a log line
naming the plane format and the framebuffer format. A linear XRGB8888 plane
with an XRGB8888 kernel picture can be mapped and flipped without a copy.

## Commands and their write gates

```text
gpu info
gpu vrammap
gpu vramtest                      # read-only candidate check
gpu vramtest --write              # dry-run, names the candidate
gpu vramtest --write --yes        # write/read 4 KiB, one-line result
gpu vramalloc 8192
gpu vramfill <id> 0x5a5aa5a5      # dry-run
gpu vramfill <id> 0x5a5aa5a5 --yes
gpu vramread <id>                 # verify
gpu bufalloc 8192 --name wm
gpu buffill <id> [--yes]
gpu bufread <id> / gpu buflist / gpu buffree <id>
gpu bufmap <id> [--yes] [--own]
gpu bufflip <id> [--yes] [--keep]
gpu bufunmap <id> [--yes]
```

`vramtest --write --yes` writes a 4 KiB pattern outside the first 16 MiB and
outside every enabled scanout, reads it back, and restores the original words.
Its one result line is `PASS addr=BAR0+0x... size=4096 write/read ok, original
restored`, or `FAIL addr=... offset=+N expected=0x... got=0x... restore=ok`.
`vramfill` and `buffill` write VRAM only with `--yes`; `bufflip`, `amdflip` and
`bufunmap` already required it. A log marker and `k_logfile_flush()` precede
every VRAM store and every GRPH write sequence. `bufmap --own` requires
`--yes` and marks the block as owned by the holder of `/dev/fb0`.

`bufmap` maps the allocation at the fixed kernel VA
`0xffffffffbd000000` (`PORT_X86_64_GPU_VRAM_SURFACE_VIRT`), plus the in-page
offset of the allocation. The read-only VRAM probe window is
`0xffffffffbc000000` (`PORT_X86_64_GPU_VRAM_VIRT`).
It prints the id/name, VA, allocation and surface sizes, stride in pixels and
bytes, `GRPH_CONTROL` depth/format/bpp, `UNP_GRPH_CONTROL` tiling, owner and
whether `present` is armed. `buflist` adds `mapped`, `owner` and `active`.
Without `--yes`, `bufmap` is mapping-only and `present` keeps the RAM copy.

## Format and safety rules

* `GRPH_PITCH` is pixels per line; `GRPH_Y_END + 1` is the visible height.
* `GRPH_CONTROL.GRPH_DEPTH/GRPH_FORMAT` names the encoding. RGB565 is depth 1,
  format 1, two bytes per pixel. Depth 2 / format 0 is the 32-bit
  ARGB8888/ABGR8888 family, depth 2 / format 1 is ARGB2101010, and depth 3 /
  format 0 is the 64-bit floating-point family. `GRPH_CONTROL` does not tell
  ARGB8888 from ABGR8888; our XRGB8888 surface is a promise about our own
  byte layout, not a claim about the panel's wire format.
* `UNP_GRPH_CONTROL` must report a linear array mode and no micro-tiling.
  A tiled or unreadable plane is refused for CPU drawing and page flipping.
* A plane whose format differs from the kernel framebuffer is refused with
  `no copy path, RAM copy stays in use`; this is the explicit answer on the
  owner's 32-bit desktop while the kernel picture is RGB565.
* Allocations start above the first 16 MiB and exclude every enabled scanout.
  A mapped or active id cannot be freed. Several blocks coexist; `buflist`
  shows each one and freeing one leaves the others intact.
* Only GRPH primary/secondary surface address and the unchanged pitch register
  are written. Timings, PLL, DDI, format, tiling and enable state are untouched.
* Flip success requires a vblank edge and address/pitch/control/tiling
  readback. The original plane is restored by default. A failed rollback is a
  hard error, not a hidden software copy.

## `k_blit_present` selection

1. The source is the exact mapped surface, the shape, stride and format match,
   the plane is still compatible, and the surface was armed with `bufmap --yes`:
   the AMD backend points GRPH at the existing VRAM block. No RAM staging or
   pixel copy is made.
2. The source is RAM, the surface is not armed, or the plane is incompatible:
   the backend restores any active VRAM plane and answers `KERR_NOTSUP`.
   `k_blit_present` performs its normal software copy into the original
   framebuffer.
3. A flip or rollback fails: a driver error is propagated. The portable blit
   layer does not hide it with a copy into a destination that may not be live.

The refusal latch is per source, so a RAM refusal does not disable a later
VRAM-surface flip.

## Lifetime of a block

`gpu bufmap <id> --yes --own` gives the block to the holder of `/dev/fb0`.
`fbdev_close` runs when a program exits (`task_exit` closes its file table)
and calls `k_blit_present_release`; after a confirmed restore, the AMD backend
unmaps the surface, frees that block and logs `released fb-owned VRAM block
id=N`. If the restore fails, nothing is freed and the mapping stays: the block
may still be live on the panel. If several programs hold `/dev/fb0`, the last
close is the release point. An unowned block is a kernel experiment: it stays
until `gpu bufunmap <id>` and `gpu buffree <id>`, and `bufunmap` never frees it.

## Owner checklist on the AMD desktop (five lines)

1. Boot Micro OS alone on the card (no `amdgpu`), with a serial log and a known
   power-cycle; record `gpu info` and `gpu vrammap` (pipe, address, pitch,
   format, bpp, tiling, `linear`).
2. Run `gpu vramtest --write`, then `gpu vramtest --write --yes`; keep the one
   line PASS/FAIL with its BAR0 address, offset, expected and got words.
3. Allocate two buffers, `gpu buffill <id> --yes` / `gpu bufread <id>` each,
   then `gpu buffree` one and confirm the other survives in `gpu buflist`.
4. Run `gpu bufmap <id> --yes --own` and `gpu bufflip <id> --yes --keep`;
   `gpu buflist` must show `active=yes`, `gpu bufunmap <id> --yes` must restore
   and unmap, and a 32-bit/tiled mismatch must be an explicit refusal.
5. End a program holding `/dev/fb0` (or `FBIO_GIVE`); the log must show
   `released fb-owned VRAM block id=N` and the id must be gone from `buflist`.

## Still needed before `wm` draws directly into VRAM

* A user-space mapping ABI that carries the VRAM buffer identity/offset (and
  the 32-bit byte order) into `FBIO_PRESENT`; the fixed VA here is kernel-only
  and `k_port_exec_contains` refuses it as a source from ring 3.
* `wm` selecting that mapping as its canvas instead of its RAM backbuffer.
* A measured RGB565-to-XRGB8888 conversion path if the owner keeps an RGB565
  canvas on the 32-bit desktop; today the honest answer is the refusal above.
* Hardware validation of alternate modes, including a linear 32-bit mode.

## Provenance

Register indices/masks and the depth/format/tiling encoding are hardware facts
from Linux v6.12 `dce_11_0_d.h`, `dce_11_0_sh_mask.h`,
`drivers/gpu/drm/amd/display/dc/dce110/dce110_mem_input_v.c` and
`drivers/gpu/drm/amd/display/dc/dc_hw_types.h` (MIT). No amdgpu code is copied;
the mapping, state machine, rollback, ownership and tests are original Micro OS
work. The live-card numbers above come from read-only `modetest` and a DRM
framebuffer-info ioctl on the host, not from Micro OS register writes.
