# Intel GPU reconnaissance: ThinkPad X1 Carbon 6, UHD 620

This is the first step of an Intel display driver: identify the card, prove
that its MMIO answers, and report the panel state that firmware left behind.
The boot probe does not set a mode, train a link, touch backlight, submit GPU
work, or change a display register. The only later write path is the
owner-confirmed `gpu flip --yes` experiment described in section 3.8.

**Status:** the laptop facts below were captured read-only over SSH. The PCI,
EDID, connector, dmesg and VBT values are measured, not predictions. The only
unmeasured part is how the same power state looks when Micro OS, without DMC,
takes over.

## 1. Provenance and licence boundary

The Linux i915 driver is GPL-licensed. It was used as a description of the
hardware, not as source to translate:

* the PCI identifiers and the Kaby Lake display topology come from Intel's
  public PCI/processor information and the public i915 device tables;
* register offsets, bit meanings, forcewake ranges and power-well layout come
  from the Intel Kaby Lake Programmer's Reference Manuals and the public i915
  register/power-management sources;
* EDID and VBT container facts come from the public standards, Intel PRMs and
  the kernel's VBT documentation.

No i915 source text, macro expression, control flow or data-structure
expression was copied into this tree. The Micro OS probe is an original,
read-only implementation. Numeric hardware facts are knowledge, not the
GPL expression that describes them. The same provenance note appears in every
new source and test file.

Primary references are listed in section 9. The supplied `docs/plan-gpu.md`
section 5 was not present at the base commit of this worktree; the boundary
above follows the rules supplied with this task.

## 2. Measured hardware

The owner captured `/tmp/opencode/laptop-recon.txt`,
`/tmp/opencode/laptop-recon2.txt` and `/tmp/opencode/x1c6-vbt.bin` on the
ThinkPad. The VBT blob stays outside the repository.

| Fact | Measured value | Evidence |
|---|---|---|
| Machine | ThinkPad X1 Carbon 6th Gen | owner |
| GPU | Intel Kaby Lake-R GT2, UHD Graphics 620 | `lspci` |
| PCI address | `0000:00:02.0`, function 0 | `lspci` |
| Vendor:device:revision | `8086:5917`, revision `07` | `lspci` |
| Subsystem | Lenovo `17aa:225c` | `lspci` |
| Class | `030000`, VGA-compatible controller, prog-if `00` | `lspci` |
| Command | `I/O+ Mem+ BusMaster+`; memory decode is already enabled | `lspci` |
| Power state | PCI PM v2 at capability `d0`; status `D0` | `lspci` |
| BAR0 | `0x2ff0000000`, 64-bit, non-prefetchable, 16 MiB | `lspci` |
| BAR2 | `0x2fe0000000`, 64-bit, prefetchable, 256 MiB (GTT) | `lspci`; not used by the probe |
| BAR4 | I/O window at `0xe000`, 64 bytes | `lspci`; not used by the probe |
| Expansion ROM | `0xc0000`, 128 KiB, **disabled** | `lspci`; the probe does not enable it |
| Display IP | **9.00, stepping C0** | live i915 dmesg plus the KBL DID/revision tables |
| Graphics architecture | Kaby Lake-R GT2, often marketed as gen 9.5; this is not the display IP version | public platform fact |
| GPU execution units | 24 EUs / 192 shaders | public platform fact; not display pipes |
| Display topology | A/B/C pipes, A/B/C/EDP CPU transcoders, up to five logical DDIs | i915 `skl_display`; runtime fuse mask was not exported by the available debugfs |
| Connectors | `eDP-1` connected; `DP-1`, `DP-2`, `HDMI-A-1`, `HDMI-A-2` disconnected | live DRM state |
| Active panel mode | `1920x1080` | live mode report |
| EDID | one valid 128-byte base block, BOE `HV140FHM-N61` | live sysfs EDID, decoded below |
| VBT | 6144-byte debugfs blob; meaningful `vbt_size` 4441; BDB version 212 | captured blob |
| DMC | `i915/kbl_dmc_ver1_04.bin`, payload v1.4 | live i915 dmesg |
| Micro OS firmware | no DMC, GuC or HuC is loaded by this probe | intentional |

The measured PCI state matches the probe's assumptions: `8086:5917` is in the
ID table, function 0 and class `03:00` match, PMCSR is D0, BAR0 is a 64-bit
memory window, and memory decode is already set. On this laptop the probe will
therefore not need its conditional PCI command write. BAR2 is read only as the
graphics-aperture base; BAR4 and the ROM are outside the probe's read set.

For the common `gpu_info`, `mmio_base` is read from BAR0. `mmio_size` is the
16 MiB identity-table value corroborated by the live `lspci`; the probe does
not write all ones to BAR0 merely to size it. After BAR2 validates,
`vram_bytes` reports the measured 256 MiB graphics aperture, not dedicated VRAM.

### 2.1 Where display 9.00 stepping C0 comes from

The live kernel reports:

```text
Found kabylake (device ID 5917) integrated display version 9.00 stepping C0
```

Kaby Lake has no display GMD_ID register to query. The version is derived in
two stages:

1. DID `5917` selects the KBL platform and the `skl_display` description,
   whose display IP is major 9, release 0.
2. PCI revision `07` indexes the KBL stepping table. Revision 7 maps to
   **display C0** and GT G0; PCI revision is not itself the IP stepping.

The earlier draft's “gen 9.5” described the graphics microarchitecture and
was wrong as a display-engine version. The code now records release 0 and
reports `display 9.00 stepping C0` for this measured PCI revision. Unknown
revisions or non-KBL families report `stepping unknown` rather than guessing.

### 2.2 Measured EDID facts

i915 read the panel EDID; Micro OS has **not** performed an AUX read. The
captured 128-byte block has a valid header and modulo-256 checksum.

| EDID field | Measured value |
|---|---|
| Version/revision | EDID 1.4 |
| Manufacturer | BOE (`09 e5`) |
| Product/model code | 1759 (`0x06df`) |
| Serial number | 0 / not supplied |
| Manufacture date | week 1 of 2016 |
| Panel names | `BOE CQ`, `HV140FHM-N61` |
| Interface | DisplayPort, digital |
| Bits per primary color | 8 bpc |
| Color formats | RGB 4:4:4 |
| Gamma | 2.20 |
| Physical image size | 31 × 17 cm |
| Preferred timing | 1920 × 1080 at 60.011884 Hz |
| Preferred pixel clock | 141.400 MHz |
| Preferred horizontal timing | active 1920, front 60, sync 32, back 130; total 2142; positive sync |
| Preferred vertical timing | active 1080, front 3, sync 6, back 11; total 1100; negative sync |
| Second timing | 1920 × 1080 at 48.009507 Hz, 113.120 MHz |
| Second horizontal timing | front 48, sync 32, back 142; total 2142; positive sync |
| Second vertical timing | front 3, sync 6, back 11; total 1100; negative sync |
| Declared range descriptor | absent |

There is no EDID monitor-range descriptor. The two DTDs span 48.009507 to
60.011884 Hz vertically, 52.810 to 66.013 kHz horizontally, and 113.120 to
141.400 MHz pixel clock; those are observed descriptors, **not** a declared
minimum/maximum range. The preferred DTD and the live 1920x1080 mode agree.

### 2.3 Measured VBT facts

The blob is not committed. Its measured identity is:

```text
size:    6144 bytes
sha256:  811a4e619a470b97072c36326c4c6ff13c13c7635a67b3fad871cfd3750380be
```

Header fields:

| Field | Value |
|---|---|
| Signature | `$VBT SKYLAKE` |
| VBT header version | 100 |
| VBT header size | 48 |
| `vbt_size` | 4441 |
| VBT checksum | valid: the first 4441 bytes sum to zero modulo 256 |
| BDB offset | 48 (`0x30`) |
| BDB signature | `BIOS_DATA_BLOCK` |
| BDB version | 212 (`0xd4`) |
| BDB header size | 22 |
| BDB size | 4392 (`0x1128`) |

Relevant blocks found by a bounded walk (offsets are from the start of the
file; size is the payload size after the three-byte block header):

| BDB | Offset | Size | Content used here |
|---:|---:|---:|---|
| 254 | `0x0046` | 234 | private/skip data |
| 1 | `0x0133` | 5 | General Features |
| 2 | `0x0170` | 309 | General Definitions and child devices |
| 12 | `0x051c` | 19 | Driver Features |
| 27 | `0x05f8` | 748 | eDP sequencing/link data; record only |
| 40 | `0x09b4` | 26 | LFP Options |
| 41 | `0x09d1` | 148 | LFP table pointers |
| 42 | `0x0a68` | 1298 | LFP timing/PNP/name tables |
| 43 | `0x0f7d` | 129 | backlight data; record only |
| 44 | `0x1001` | 22 | power-conservation data; record only |
| 46 | `0x101a` | 304 | chromaticity data |

BDB 2 declares 38-byte child-device records. Three records have a nonzero
device type:

| Handle | Type | DVO port | AUX | DDC pin | Wiring | Interpretation |
|---|---|---|---|---:|---:|---|
| `0x0008` | `0x1806` | DPA (`10`) | A (`0x40`) | 3 | 0 | internal LFP/eDP on DDI A |
| `0x0004` | `0x60d6` | DPB (`7`) | B (`0x10`) | 5 | 1 | external DP dual-mode |
| `0x0040` | `0x60d6` | DPC (`8`) | C (`0x20`) | 4 | 2 | external DP dual-mode |

The other five records have device type zero and are not routing evidence.
The two DP dual-mode records explain why DRM exposes both DP and HDMI
connector names for external outputs.

BDB 40 sets `panel_type = 2` (`panel_type2 = 0`). The selected LFP entry 2,
however, contains placeholder/default data:

| Field | VBT entry 2 |
|---|---|
| Fixed timing size | 1024 × 768 |
| DTD pixel clock | 65.000 MHz |
| DTD horizontal | active 1024, blank 320 |
| DTD vertical | active 768, blank 38 |
| DTD sync offsets | horizontal 24/136, vertical 3/6 |
| PNP product/serial | 3 / 3 |
| PNP date | week 0, 2002 |
| Panel name | `LFP_PanelName` |

This does **not** match the measured BOE EDID or the active 1920x1080 mode.
The VBT is still useful for DDI/AUX routing and board capabilities, but its
panel DTD/PNP/name placeholders must not override EDID and live timing. This
is exactly why the first driver step records evidence instead of replaying
VBT register values.

### 2.4 DMC is present on Linux, absent from this probe

The live i915 driver loaded `i915/kbl_dmc_ver1_04.bin` (v1.4). On gen9 the
DMC participates in display DC-state and power-well handling. Micro OS does
not load or emulate it in this probe, so:

* DC5/DC6 transitions and some power-domain requests are outside this step;
* status registers may reflect firmware/DMC-owned state rather than a state
  Micro OS can change;
* the probe only reads state and stops if PW1 is not already on;
* a later modesetting step will need an explicit decision about DMC, not an
  implicit assumption that Linux left every display resource available.

## 3. The safe handshake

### 3.1 Order of operations

The probe performs these steps and stops at the first unsafe or impossible
one:

1. Find a supported Intel PCI ID and require function 0, class `03:00`.
2. Read vendor, device, revision, subsystem IDs, command and status words.
3. Walk the PCI capability list **read-only** and require PMCSR to report D0.
   D1/D2/D3 is a skip: no MMIO is touched and no power-state write is made.
4. Set only `PCI_COMMAND.MemorySpace` if firmware left it clear. Bus mastering
   and I/O decoding are not requested.
5. Read BAR0 as 32-bit or 64-bit memory. Do not size-probe it with all ones;
   the probe already knows the 16 MiB register/GGTT-alias window it needs.
6. Map that window as uncached MMIO, then read only the registers in section
   3.3. There is no forcewake request and no display power-well request.
7. Return the shared `GPU_PROBE_*` result; the dispatcher maps no known card to
   `KMOD_SKIP` and formats the common report line.

The existing x86-64 API at this base commit is
`port_x86_64_paging_map_mmio()`. The task's proposed portable name
`k_port_mmio_map()` does not exist here; the future dispatcher integration
should either keep this port call or land the portable API first and adapt
this one call.

### 3.2 PCI configuration space

All offsets are byte offsets in PCI configuration space.

| Offset | Read | Meaning |
|---:|---|---|
| `0x00` | 16-bit vendor | `0x8086` |
| `0x02` | 16-bit device | `0x5917` for UHD 620; nearby IDs are in the probe table |
| `0x04` | 16-bit command | preserve all bits; only memory decode may be enabled |
| `0x06` | 16-bit status | capability-list present and other firmware state |
| `0x08` | 8-bit revision | the low byte of the dword at `0x08` |
| `0x0a` | class register | require base class `0x03`, subclass `0x00` |
| `0x10/0x14` | BAR0 low/high | type bits `01` mean I/O, `10` mean 64-bit memory |
| `0x2c/0x2e` | subsystem vendor/device | board identity, useful for VBT correlation |
| `0x34` | capability pointer | bounded, read-only walk to the PCI Power Management capability |
| PM cap + `0x04` | PMCSR | bits 1:0 must be zero (D0) before any MMIO read |

A missing or malformed power capability is not treated as D0. It is safer to
skip than to issue a read to a device whose power state is unknown.

### 3.3 BAR0 MMIO reads

These are the only display MMIO reads in the first probe. Offsets are relative
to BAR0. None of them is written.

| Offset | Name | Why it is useful | Requirement |
|---:|---|---|---|
| `0x42000` | `SKL_FUSE_STATUS` | PG0/PG1/PG2 distribution status; corroborates display power state | no GT forcewake |
| `0x44440` | `GEN8_DE_PORT_ISR` | read-only display-engine port interrupt state; a liveness sample, not a version register | display power domain must be awake |
| `0x44460` | `GEN8_DE_MISC_ISR` | read-only miscellaneous display interrupt state; another liveness sample | display power domain must be awake |
| `0x45400` | `HSW_PWR_WELL_CTL1` | BIOS request/state view for KBL power wells | always-on control bank |
| `0x45404` | `HSW_PWR_WELL_CTL2` | driver request/state view; PW1 state is bit 28, PW2 state bit 30 | always-on control bank |
| `0x6f400` | `TRANS_DDI_FUNC_CTL_EDP` | bit 31 says whether the eDP transcoder is enabled; bits 12:14 name its input pipe | read only; i915 treats this EDP register as always-on |
| `0x6f000` | `TRANS_HTOTAL_EDP` | low 16 bits are active width minus one | read only when PW1 is on and eDP is active |
| `0x6f00c` | `TRANS_VTOTAL_EDP` | low 16 bits are active height minus one | same |
| `0x7f008` | `TRANSCONF_EDP` | bit 31 confirms that the EDP pipe/transcoder is enabled | same |

The power-well indices on KBL are MISC_IO 0, DDI_A/E 1, DDI_B 2, DDI_C 3,
DDI_D 4, PW1 14 and PW2 15. Each index has a request bit at `2 + 2*index`
and a state bit at `2*index`. Reading these bits does not request or release
a well.

The probe checks PW1 state before touching display-domain registers. If PW1
is off, it stops with `KMOD_DEGRADED`; it does not raise the well. A read of
`0xffffffff` from the sampled MMIO is treated as a failed/unreliable BAR, not
as a successful handshake. Zero interrupt status is valid and is not an
error.

### 3.4 Forcewake is not a display power well

On gen9 the i915 forcewake table covers the low GT/render/media register
ranges below `0x40000`; the display register ranges used here do not require
a GT forcewake reference. They can still be inaccessible when their display
power domain is down. That is why the probe reads `HSW_PWR_WELL_CTL*` and
`SKL_FUSE_STATUS` first.

Do **not** write `FORCEWAKE*`, `HSW_PWR_WELL_CTL*`, `GEN9_CLKGATE_DIS*`,
`DC_STATE_EN`, PCODE requests or DMC controls to make a read work. Forcewake
has request/acknowledgement ownership and delayed-release rules; an
unbalanced request can keep a power domain awake or leave the GPU in a state
the firmware does not expect. `SKL_DFSM` at `0x51000` is useful for pipe fuse
information, but it lies in a GT forcewake range and is deliberately **not**
read by this probe.

### 3.5 Display version and topology

Kaby Lake does not expose a display GMD_ID version register. The live i915
result, `display version 9.00 stepping C0`, is derived from DID `5917` plus
PCI revision `07`, as recorded in section 2.1. The Micro OS ID table therefore
stores display IP major 9 and release 0; a small KBL revision table supplies
the display stepping without pretending that PCI revision is the IP step.

The topology recorded by the table is three display pipes, four CPU
transcoders including EDP, and five possible logical DDIs. The measured VBT
and DRM connectors narrow the board routing to eDP/DDI A and DP dual-mode
DDI B/C; the runtime fuse mask was not available in the captured debugfs.

The eDP timing read reports the **active firmware-left timing**, not a claim
that EDID was read and not a new mode. It is deliberately labelled `panel
unknown` when eDP is inactive, the power domain is unavailable, or the timing
fields are implausible.

### 3.6 Stage 1: the flat GGTT behind the active plane

The gen8 global page table is not a four-level table and BAR2 is not its
storage. BAR2 is the 256 MiB graphics aperture: byte offset `N` in that window
is the memory described by global PTE `N / 4096`. The PTE array is reached
through the BAR0 alias at `BAR0 + 8 MiB`; each entry is a 64-bit, 4 KiB page
translation with the present bit at bit 0 and the physical page in bits 51:12.
`GEN8_PDP_*` and the PDE walk belong to private PPGTT address spaces, so this
stage does not read or program them.

The read-only path selects the primary plane of the pipe named by the active
eDP transcoder, reads `PLANE_CTL`, `PLANE_STRIDE`, `PLANE_SIZE` and `PLANE_SURF`,
then resolves every PTE covered by the surface. Linear strides use 64-byte
units; Kaby Lake X/Y tiles use 512-byte units; Yf uses 128-byte units for the
supported 16/32-bit formats. The log reports the first physical page, visible
size, decoded pitch, allocated byte count and whether the physical pages are
contiguous. No PTE or display register is written.

The parser is covered by synthetic X-tiled, linear RGB565, Yf-padding,
fragmentation and invalid-entry cases in `tests/cases/test_gpu_intel_ggtt.c`.
The live physical address and pitch remain unmeasured until the owner boots
this image on the ThinkPad; QEMU has no Intel card and the shared dispatcher
must keep reporting `KMOD_SKIP`.

### 3.7 Stage 2: one RAM page through the aperture

`gpu ggtt-test` first excludes the active framebuffer, every enabled universal
plane and every enabled cursor. It then requires a run of identical valid PTEs
with the same scratch value on both guard entries; a zero or unguarded range is
not accepted. One page from the kernel heap is aligned and its bus physical
address is installed in the chosen slot. The command writes a deterministic
word pattern through BAR2, reads it back through BAR2 and through the CPU page,
checks both neighboring PTEs, and restores the original PTE on every exit path.
No display register is written and no firmware, forcewake or power-well request
is made.

### 3.8 Stage 3: confirmed same-pixel plane flip

`gpu flip` is a read-only dry-run. Only `gpu flip --yes` enters the write path,
and only for the measured laptop identity `8086:5917` revision `07` with
subsystem `17aa:225c`. The command re-probes the display, excludes every live
surface, reserves a guarded GGTT run, allocates a physically contiguous RAM
copy, copies the source through BAR2 and compares every byte before touching a
display register.

At the next pipe frame counter change it writes the saved `DSPCNTR`/`PLANE_CTL`,
`PLANE_STRIDE` and the new `PLANE_SURF`. Two frame changes later it requires
`PLANE_SURFLIVE` to name the new surface and compares the copied pixels again.
It then restores the old surface at the next frame and requires `SURFLIVE` to
return to the old address. The one-line rollback is: write `PLANE_SURF(old)`
first, then `PLANE_STRIDE(old)` and `DSPCNTR(old)`, then restore the GGTT run
only after `SURFLIVE` confirms the old surface. If the plane restore itself
fails, or the old surface does not become live, the new GGTT mapping and RAM
allocation are intentionally kept alive so a scratch mapping cannot turn the
screen black.

This does not set a mode, train DDI/eDP, change backlight or load DMC/GuC/HuC.
A probe that cannot already see PW1 and the active eDP pipe refuses the flip.
The remaining live unknown is whether the firmware-left display state survives
the Micro OS handoff without DMC; that is an owner-confirmed laptop test, not a
QEMU test.

Owner checklist:

1. Boot the signed image with serial/power recovery ready; record the boot
   `gpu: ggtt framebuffer ...` line and run `gpu info` for the shared report.
2. Run `gpu ggtt-test`; require `PASS` and no neighboring-entry error.
3. Run `gpu flip`; check the source/target, dimensions and pitch in the dry-run.
4. Run `gpu flip --yes`; require `copy verified` and `restored original surface`.
5. If the image is black/garbled or any error appears, stop, save the log and
   power-cycle; do not repeat the experiment.

### 3.9 Stage 4: the compositor presents two RAM surfaces

`/bin/wm` allocates two RGB565 surfaces. It draws damage and the pointer into
the back surface, calls `FBIO_PRESENT`, and changes its drawing target only
when the kernel answers 0 (the device showed that surface). The first
successful present seeds the spare surface once; later presents only flip.
The kernel's `k_blit_present` asks the registered device callback once. A
missing callback, an incompatible format, no guarded GGTT run or a page which
does not translate answers with the software copy, and the refusal is latched
for that registration. QEMU and AMD therefore keep the damage-copy path.

The Intel callback is registered only for the measured ThinkPad identity and
only when the active primary plane is already linear RGB565 without surface
flags, rotation or compression. It reserves two guarded GGTT runs, translates
each RAM page through the caller's page tables, holds a reference to the
physical frame, installs the non-contiguous pages, flushes the visible CPU
cache lines and writes `PLANE_SURF`. It waits for `PLANE_SURFLIVE` before the
old front may become the next back surface. `FBIO_GIVE` and the last close
restore the original plane before the two mappings and frame references are
released. If plane rollback itself fails, the references and GGTT mappings
stay alive so a departing program cannot have its frame reused under the
display engine.

The reconnaissance measured a firmware-left **X-tiled XRGB8888** plane. A boot
which still has that state cannot show the compositor's linear RGB565 surface
by changing only `PLANE_SURF`; it must take the quiet copy fallback. The boot
line `gpu: Intel page-flip backend ready (... linear RGB565)` is therefore the
first capability check, not a promise that every later GGTT allocation will
succeed. No mode, DDI/eDP, watermark, backlight or firmware state is changed.

Owner checklist for the automatic present path:

1. Boot with serial and power recovery ready. Save the `gpu:` lines. Require
   `gpu ggtt-test` to pass before starting the desktop; if the page-flip
   backend line is absent, record that the measured plane is incompatible and
   expect the copy path rather than a failure.
2. Start `/bin/wm`. The desktop, pointer, window moves, closes and the clock
   must remain visible with no black frames, corrupted edges or stale cursor.
3. Exercise damage for at least a minute: move and raise windows, type in the
   editor, move the pointer and press F12 between actions. Compare the serial
   `wm: the first frame took ... ns by TSC`, `wm: the first present took ... ns
   by TSC` and `wm: present: ...` lines with the QEMU numbers in
   `docs/bench.md`.
4. Close every window (or Alt+F4 each one). The shell grid must return, which
   exercises `FBIO_GIVE`, plane restore and GGTT cleanup before the RAM
   surfaces are unmapped. Start and stop the desktop a second time.
5. Reboot once more and confirm the display still comes up. If any frame is
   black/garbled, a rollback error appears, or the shell does not return, stop,
   save the complete serial log and power-cycle; do not keep testing the flip.

Vsync is still a blocking wait for `SURFLIVE` in this stage. There are two
surfaces, not three: the single WM task cannot compose the next frame while it
waits for the current flip, so a later stage can overlap composition with the
wait or add a third surface. The per-frame 2 MiB copy is gone only on the
compatible Intel path; the first successful present still seeds the spare
surface once.

## 4. EDID through AUX/DDC: the next, separate step

Reading EDID is useful, but it is not part of this read-only probe. Both AUX
and GMBUS reads require writes to controller registers, so they belong behind
an explicit later gate. The facts in section 2.2 came from the EDID that the
live i915 driver read; Micro OS has not issued an AUX transaction.

For the internal panel, the measured path is DisplayPort I2C-over-AUX on AUX A
(eDP/DDI A): the VBT child device names DPA and AUX A. On gen9 the AUX A
control register is BAR0 `0x64010`, and its five little-endian data words are
`0x64014..0x64024`. Other channels have different control banks; do not assume
A for an external connector without VBT/DRM mapping.

A safe future transaction is:

1. Confirm HPD/panel presence, AUX channel mapping, D0 and the AUX power domain.
2. Use I2C-over-AUX, never native DPCD transactions, to address the EDID sink
   at 7-bit `0x50` (8-bit write `0xa0`, read `0xa1`).
3. Write offset zero, then read one 128-byte base block in bounded chunks
   (the hardware data registers hold at most 20 bytes per AUX message). Set
   MOT on all but the final transaction and finish with MOT clear.
4. Check the header `00 ff ff ff ff ff ff 00`, EDID version/revision, block
   length and the modulo-256 checksum. Only if extensions are needed, select
   the segment pointer at `0x30` and validate each extension block.
5. Never write an EDID byte. A read operation uses the sink's read direction;
   an accidental `0xa0` data write can damage a panel's stored EDID.

GMBUS is the DDC/I2C alternative for HDMI/DVI-style connectors. Its gen9
register bank is `0x5100` (port/rate), `0x5104` (command), `0x5108` (status),
`0x510c` (four data bytes), `0x5110` (interrupt mask) and `0x5120` (byte
index). It also requires control writes and power-domain ownership. i915 uses
AUX for DisplayPort/eDP EDID; GMBUS is not a substitute for the internal eDP
path.

What EDID gives the driver:

* manufacturer ID (bytes 8-9), product code (10-11), serial (12-15), and
  manufacture week/year (16-17);
* physical size, gamma, supported color formats and feature flags;
* the preferred detailed timing, normally in the first 18-byte descriptor at
  bytes 54-71: active size, porches, sync widths/polarities and pixel clock;
* monitor name and range descriptors in bytes 72-125;
* extension count at byte 126, with CTA-861/DisplayID data when present.

EDID does not prove which connector is active, does not replace VBT board
routing, and may be absent or synthesized. Cross-check it against the DRM
connector, VBT panel entry and firmware-left transcoder timing.

## 5. VBT and VBIOS

### 5.1 Where the VBT lives

The Video BIOS Table is board-specific display configuration. Intel's loader
looks first in the ACPI graphics OpRegion and falls back to the PCI expansion
ROM on older systems. The VBT starts with `$VBT`; it is not the whole VBIOS and
it is not GuC/HuC firmware.

For reconnaissance on Linux, the least invasive source is i915's read-only
debugfs copy. The measured laptop blob and its parsed fields are in section
2.3; the binary is deliberately not in this repository:

```sh
sudo cat /sys/kernel/debug/dri/0000:00:02.0/i915_vbt > /tmp/x1c6-vbt.bin
sha256sum /tmp/x1c6-vbt.bin
```

The live PCI expansion ROM is present but disabled, so enabling it is neither
necessary nor part of this probe. If debugfs is unavailable, a read-only
ROM/OpRegion dump is a fallback, but do not flash, unlock or write either ROM;
record the source and hash of every blob.

### 5.2 Container validation

Before interpreting a VBT:

* require at least the fixed header and the four-byte `$VBT` signature;
* read the little-endian header version, header size, total VBT size and
  checksum; reject sizes outside the captured buffer;
* read the BDB offset at VBT byte `0x1c` and require the BDB signature
  `BIOS_DATA_BLOCK` at that offset;
* read the BDB version/header size/BDB size and walk blocks within the BDB;
* a normal block has a one-byte ID, a little-endian 16-bit size and payload;
* validate every offset and size before following pointers. Unknown blocks
  are skipped, not guessed. Block 53 has a version-dependent size layout and
  is not needed for this first step.

### 5.3 Blocks needed first

| BDB ID | Content | Why it matters now |
|---:|---|---|
| 1 | General Features | panel fitting and platform feature bits |
| 2 | General Definitions | boot display, child-device size and child-device records |
| 11 | Child Device Table (legacy layout) | connector/device handles on older VBTs |
| 12 | Driver Features | boot mode and primary-LFP policy |
| 27 | eDP | panel power sequence and link-training parameters; **record only**, do not execute |
| 40 | LFP Options | panel type, color depth and panel-channel capabilities |
| 41 | LFP Data Pointers | offsets/sizes for timing, PNP ID and panel-name tables |
| 42 | LFP Data | fixed timing/register values, DTD and panel PNP ID |
| 43 | LFP Backlight | controller type/frequency; **record only**, never touch PWM/backlight |
| 44 | LFP Power Conservation | power-feature bits; **record only** |
| 58 | Generic DTD (BDB 229+) | modern internal-panel timing; prefer it when present, with legacy fallback |

The first driver step needs only: BDB version, panel type, child-device routing
(eDP on which DDI/AUX), panel name/PNP ID, native timing and color depth. Power
sequences, GPIO, PWM, link-training values and register-value blobs are
reconnaissance material, not commands to replay. On this laptop the VBT panel
entry is placeholder data (section 2.3), so EDID and active timing take
precedence for panel identity while VBT remains the routing source.

VBT child-device fields are versioned. A field introduced after the captured
BDB version must not be interpreted. In particular, AUX/DDC mapping and panel
type must be read according to the BDB version, not by copying a modern
structure over an old blob.

## 6. What must not be done

The first probe must not:

* write any BAR0 display register, including interrupt status/acknowledgement
  registers, clock gates, power wells, forcewake, DC state, PCODE or DMC;
* enable/disable a pipe, DDI, transcoder, PLL, PHY, AUX, GMBUS or panel power;
* write timing, plane, stride, address, flip, watermark, color or backlight
  registers;
* train DisplayPort, change lanes/rates/voltages, or issue an EDID write;
* load GuC/HuC/DMC firmware, submit a batch buffer, reset the GPU or engine;
* change PCI PMCSR, bus-master enable, ROM enable, expansion ROM bytes or
  firmware variables;
* assume a zero/`0xffffffff` MMIO value is healthy, or retry a hung access in
  a tight loop without a platform fault policy.

Memory decode is the only PCI control write: it enables the BAR already
assigned by firmware. It does not change a display mode.

## 7. A bricked screen and recovery

“Bricked” here means the panel or display output is unusable; the laptop's
boot flash and recovery path should still exist.

Symptoms include a permanently black panel, backlight off, frozen/garbled
pixels, loss of the internal panel after reboot, a kernel hang on the first
MMIO access, or a machine that still answers serial/SSH but has no display.
An external monitor can help distinguish panel-path failure from a whole-GPU
failure, but recovery must not depend on the owner's single internal screen.

Recovery procedure:

1. Stop the experiment at the first abnormal read/hang. Do not poke more
   registers and do not repeatedly boot the suspect image.
2. If the machine responds over SSH or serial, save `dmesg`, the exact image
   hash and the last probe line, then power it off. Otherwise hold the power
   button until it powers off.
3. Disconnect AC, USB-C docks, DisplayPort/HDMI and external displays. Power
   off fully, wait several seconds, then boot the internal panel alone.
4. Boot a known-good kernel/initrd from GRUB's advanced menu. On a Linux host,
   `nomodeset` is a last-resort diagnostic, not a fix for Micro OS. For Micro OS,
   keep a signed known-good image and a boot choice that does not contain the
   experiment.
5. If the panel remains black but serial/SSH works, stop. Do not use random
   MMIO, BIOS ROM writes, EDID writes or forcewake toggles as recovery tools.
6. A BIOS/firmware reinitialization or vendor recovery may restore the display,
   but it is an owner-approved last step. Preserve VBT/EDID/dmesg evidence
   before any firmware reset.

Before a live run, arrange SSH from another machine (or serial), a way to
power-cycle the laptop, and preferably an external monitor. Save the VBT and
EDID hashes before testing so a later display can be compared with a known
state.

## 8. Live-laptop results and remaining check

The five decision facts are now recorded:

1. **PCI identity and decode:** `0000:00:02.0`, `8086:5917` rev `07`, Lenovo
   `17aa:225c`, class `030000`, BAR0 `0x2ff0000000`/16 MiB, `Mem+`, D0.
2. **Display topology:** display IP 9.00 stepping C0; VBT routes eDP to DDI A /
   AUX A and external dual-mode outputs to DDI B/C; DRM reports eDP-1 connected
   and four external connectors disconnected. Runtime fuse masks were not
   exported by the available debugfs.
3. **Panel identity:** BOE model 1759, `HV140FHM-N61`, EDID 1.4, 8 bpc,
   preferred 1920x1080@60.011884 Hz; live mode 1920x1080.
4. **VBT board data:** 6144-byte blob, SHA-256 in section 2.3, BDB 212,
   panel type 2, three meaningful child devices; panel DTD/PNP/name entries are
   placeholders and do not match EDID.
5. **Power handshake under Linux:** D0 and memory decode are confirmed. The
   remaining unknown is the PW1/DDI state when Micro OS starts without DMC; the
   probe reads that state and never raises a power well.

The read-only commands used for the capture are kept below as a reproducible
checklist (paths may use a DRM minor other than 0):

```sh
uname -a
sudo lspci -D -nn -vv -s 0000:00:02.0
sudo cat /sys/bus/pci/devices/0000:00:02.0/vendor
sudo cat /sys/bus/pci/devices/0000:00:02.0/device
sudo cat /sys/bus/pci/devices/0000:00:02.0/revision
sudo cat /sys/bus/pci/devices/0000:00:02.0/subsystem_vendor
sudo cat /sys/bus/pci/devices/0000:00:02.0/subsystem_device
sudo cat /sys/bus/pci/devices/0000:00:02.0/class
sudo cat /sys/bus/pci/devices/0000:00:02.0/resource
sudo xxd -g1 -l 256 /sys/bus/pci/devices/0000:00:02.0/config

for c in /sys/class/drm/card*-*/status; do echo "$c: $(cat "$c")"; done
find /sys/class/drm -maxdepth 2 -type f -name edid -print
for e in /sys/class/drm/card*-eDP-*/edid; do
    [ -s "$e" ] || continue
    echo "== $e =="
    sudo cat "$e" > /tmp/panel.edid
    sha256sum /tmp/panel.edid
    if command -v edid-decode >/dev/null; then edid-decode /tmp/panel.edid; fi
done

ls -l /sys/kernel/debug/dri/*/i915_vbt /sys/kernel/debug/dri/*/i915_display_capabilities 2>/dev/null || true
sudo cat /sys/kernel/debug/dri/0000:00:02.0/i915_vbt > /tmp/x1c6-vbt.bin
sha256sum /tmp/x1c6-vbt.bin
sudo dmesg | grep -Ei 'i915|drm|gpu|edid|vbt|dmc' | tail -80
```

Do not run `intel_reg_dumper`, `devmem`, EDID writers, VBIOS flashers or
forcewake debug tools for this reconnaissance.

### 8.1 Shared GPU handshake contract

The release branch now owns `port_x86_64_gpu.h`, `struct gpu_info`,
`enum gpu_probe_result`, the vendor table and the single `gpu` kmod/shell
registration. `gpu_intel.h` contains only the Intel identity table and a
`struct gpu_intel_info` wrapper whose first member is the shared `gpu_info`;
there is no fallback copy of the common structure.

```c
enum gpu_probe_result gpu_intel_probe(const struct port_x86_64_pci_device *pci,
                                      struct gpu_info *out);
```

The dispatcher supplies the PCI function. The probe returns `GPU_PROBE_NONE`
for another vendor, `GPU_PROBE_UNKNOWN_DEV` for an unknown Intel DID,
`GPU_PROBE_DEGRADED` when the read-only power/display handshake is incomplete,
`GPU_PROBE_FAIL` for malformed/non-answering hardware, and `GPU_PROBE_OK` only
after the firmware-left framebuffer has been resolved.

| Common field | Source |
|---|---|
| `present`, `known`, `probe_result` | shared dispatcher/probe outcome |
| `vendor`, `device`, `revision`, `bus`, `slot`, `function` | PCI configuration read |
| `mmio_base` | BAR0 read |
| `mmio_size` | 16 MiB identity-table value, corroborated by live `lspci`; no BAR-size write |
| `vram_bytes` | 256 MiB graphics aperture after BAR2 validates |

The Intel-only tail holds subsystem IDs, power-well/ISR samples, the active
pipe, plane registers and the decoded GGTT framebuffer. The common `gpu`
command forwards only `ggtt-test` and `flip [--yes]` through the weak
`gpu_intel_command()` hook; no second `gpu` command is registered. No display
register is written by the probe, no forcewake is requested, and no power well
is raised.

On a successful live handshake the single fact line is:

```text
gpu: Intel UHD 620 (8086:5917 rev 07) at 00:02.0, mmio 0x2ff0000000+16M, display 9.00 stepping C0, panel 1920x1080
```

The `display 9.00 stepping C0` portion is the legitimate DID/revision-derived
conclusion explained in section 2.1. If the eDP timing is unavailable, the
same line ends in `panel unknown`; EDID is not claimed.

After the shared dispatcher registers the module, `gpu` and `modules` should
add the health line:

```text
kmod: gpu ok
```

### 8.2 Live check script

Boot the signed Micro OS USB image, wait for its shell, then run this seven-line
capture on the connected host (use the actual USB-serial port):

```sh
#!/bin/sh
set -eu
# The laptop is already booted into Micro OS and shows its shell.
PORT=${1:-/dev/ttyACM0}
OUT=$(mktemp)
python3 tools/console.py "$PORT" 12 --send gpu modules >"$OUT"
tail -24 "$OUT"
```

Expected markers are the `gpu:` fact line and `kmod: gpu ok`; the `modules`
table should also show `gpu` in state `ok`. If the console is VGA-only, run the
same two commands and photograph the last screen lines instead of using serial.

## 9. Sources

* Lenovo PSREF, ThinkPad X1 Carbon 6th Gen platform specifications:
  <https://psref.lenovo.com/product/ThinkPad/ThinkPad_X1_Carbon_6th_Gen>
* Intel Graphics for Linux Programmer's Reference Manuals index (Kaby Lake
  volumes): <https://cdrdv2-public.intel.com/772631/graphics-for-linux_developer-reference_1.0-772629-772631.pdf>
* Linux v6.6 i915 PCI IDs:
  <https://github.com/torvalds/linux/blob/v6.6/include/drm/i915_pciids.h>
* Linux v6.6 display topology:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_display_device.c>
* Linux v6.6 PCI-revision-to-display-stepping table:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/intel_step.c>
* Linux v6.12 register definitions:
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_reg.h>
* Linux v6.12 GGTT layout and programming:
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/intel_gtt.h>
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/intel_ggtt.c>
* Linux v6.12 universal-plane registers and programming:
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/display/skl_universal_plane_regs.h>
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/display/skl_universal_plane.c>
* Linux v6.12 cursor registers:
  <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/display/intel_cursor_regs.h>
* Linux v6.6 forcewake ranges:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/intel_uncore.c>
* Linux v6.6 power wells:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_display_power_well.c>
* Linux v6.6 AUX registers:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_dp_aux_regs.h>
* Linux v6.6 GMBUS registers:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_gmbus_regs.h>
* Kernel VBT documentation:
  <https://docs.kernel.org/gpu/intel-display/vbt.html>
* Linux v6.6 VBT definitions:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_vbt_defs.h>
