# Intel mode and backlight: research plan only (Intel lane, 27 November 2026)

This page records a plan. **Nothing here writes a display register.** The
firmware already leaves the ThinkPad X1 Carbon Gen 6 panel at 1920x1080, eDP
connected through DDI A / AUX A, with a linear XRGB8888 primary plane and
backlight on. Micro OS currently inherits that state and only reads it. The
lane that produced this page does not set a mode, train a link or touch PWM.

Sources are knowledge only, not code: Intel gen9 display register definitions
as described by `drivers/gpu/drm/i915/i915_reg.h`,
`drivers/gpu/drm/i915/display/intel_dpll_mgr.c`,
`drivers/gpu/drm/i915/display/intel_ddi.c`,
`drivers/gpu/drm/i915/display/intel_panel.c` and the VBT/EDID evidence in
`docs/ports/x86/gpu-intel-recon.md` (GPL-2.0, Linux v6.12). Every line of Micro
OS code in this lane was written for Micro OS (MIT); no i915 source was copied.

## Why no write is part of this step

The recovery procedure in `gpu-intel-recon.md` section 7 is the reason: a wrong
PLL, DDI or backlight write can leave the only panel black until a power cycle,
and a wrong link-training sequence can make the panel unusable after reboot.
The wm present path does not need a new mode; it needs a plane whose format and
pitch it can flip. On this laptop that plane already exists. Mode setting is a
separate feature with its own ADR, its own test gate and an owner who can
power-cycle the machine.

## What is known from the read-only capture

* The panel is BOE `HV140FHM-N61`, EDID 1.4, preferred and live 1920x1080 at
  about 60.012 Hz, 8 bpc. VBT is BDB 212; its panel DTD/name/PNP entries are
  placeholders, so EDID wins for timing and VBT wins for routing.
* eDP is transcoder `TRANS_DDI_FUNC_CTL_EDP`; its enable bit and input-pipe
  bits are read but never changed. DDI A / AUX A is the internal path.
* The active primary plane is linear XRGB8888, `PLANE_CTL 0x04000000`,
  `PLANE_STRIDE 0x780`. The dry-run reports `plane is already linear; no
  register write needed` and `--yes` only verifies a device present before
  releasing its temporary mapping.
* PW1 and DDI wells are read in D0; the probe never raises a well, loads DMC,
  or requests forcewake.

## The read-only research sequence

1. **Current clock tree.** Read and log the eDP transcoder's PLL selection,
   WRPLL control/status and the enabled DPLL registers; derive the actual
   pixel clock from the live 1920x1080 timing instead of from VBT. Confirm
   whether the firmware uses a WRPLL or a shared DPLL. No enable bit is set.
2. **Current link.** Read `DDI_BUF_CTL`, `DDI_FUNC_CTL`, the link-training
   registers and AUX lane/rate state. Compare them with the EDID's preferred
   mode. This tells us what a future modesetter would have to reproduce, and
   whether the firmware-left state is enough for a mode that Micro OS wants.
3. **Timing and watermarks.** Read the pipe's HTOTAL/Hblank/VSYNC/Vblank and
   the watermark planes. A future write must recompute watermarks before a
   plane is enabled; an incomplete write here is more dangerous than no write.
4. **Backlight.** Read the PCH PWM control and duty registers and the panel
   power control/status. Record the current duty/frequency as the value a
   future "restore" path must write back. Do not change PWM or panel power.
5. **Decision gate.** A separate ADR must name the exact registers, the
   order of writes, the rollback, and the owner test with SSH/serial and a
   power cycle ready. Until then this lane stays read-only.

## What would make mode setting worthwhile

Only a feature that needs a new mode: a panel without firmware timing, an
external display, a refresh-rate change, or a resume path after a real deep
sleep. A page flip of an already-linear plane needs none of them. The present
work in `docs/bench.md` is therefore the priority; this page is the map for
the later feature, not a promise of one.
