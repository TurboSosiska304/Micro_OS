# Intel GPU reconnaissance: ThinkPad X1 Carbon 6, UHD 620

This is the first step of an Intel display driver: identify the card, prove
that its MMIO answers, and report the panel state that firmware left behind.
It does not set a mode, train a link, touch backlight, submit GPU work, or
change a display register.

**Status:** public facts and register knowledge only. Every machine-specific
value below is marked **confirm on the laptop** until the owner supplies the
read-only `lspci`, EDID, VBT and i915 diagnostics listed in section 8.

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

## 2. Hardware hypothesis

| Fact | Expected value | Confidence |
|---|---|---|
| Machine | ThinkPad X1 Carbon 6th Gen, type 20KH/20KG | owner fact |
| GPU | Intel UHD Graphics 620, GT2, Kaby Lake-R | public fact |
| PCI vendor:device | `8086:5917` | **confirm on live `lspci`** |
| PCI revision | example `07`; the probe accepts and reports the actual byte | **confirm on live `lspci`** |
| Subsystem | Lenovo vendor and board-specific subsystem ID | **confirm on live `lspci`** |
| PCI address | normally `00:02.0`, function 0 | **confirm on live `lspci`** |
| Class | VGA-compatible display, `03:00:00` | **confirm on live `lspci`** |
| BAR0 | 64-bit memory window; the probe needs only its first 0x80000 bytes | **confirm base, type and size on the laptop** |
| Graphics microarchitecture | gen 9.5 / Kaby Lake-R GT2 | public fact |
| i915 display IP version | 9; “9.5” is the platform/marketing generation, not a separate display IP release | confirmed by the i915 KBL mapping |
| GPU execution units | 24 EUs / 192 shaders | public fact; not display pipes |
| Display pipes | A, B, C: three | i915 `skl_display` topology; **confirm fuses/runtime mask** |
| CPU transcoders | A, B, C and EDP: four | i915 `skl_display` topology; **confirm runtime mask** |
| Logical DDI ports | A through E: up to five | silicon topology, not five sockets |
| Board routing | expected eDP on A, external DP/HDMI on the remaining wired DDIs | **confirm from VBT child devices and DRM connectors** |
| Internal panel | one of the X1C6 FHD/WQHD/HDR options; FHD 1920x1080 is not guaranteed | **confirm EDID and VBT** |
| Firmware | no GuC or HuC is loaded by this probe | intentional |

Lenovo's PSREF lists 1920x1080, 2560x1440 and HDR 2560x1440 panel options for
this generation. A machine name is therefore not a panel identity. The EDID
manufacturer/product fields and the VBT panel entry are the evidence.

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
   the probe already knows the small register range it needs.
6. Map the 2 MiB page containing BAR0 as uncached MMIO, then read only the
   registers in section 3.3. There is no forcewake request and no display
   power-well request.
7. Report one line. With no supported card, return `KMOD_SKIP` and say so.

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
| `0x44460` | `GEN8_DE_MISC_IIR` | read-only miscellaneous display interrupt state; another liveness sample | display power domain must be awake |
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

### 3.5 Display generation and topology

Kaby Lake does not expose a display GMD_ID version register. The i915 display
probe maps KBL PCI IDs to the gen9 display description. The Micro OS ID table
therefore records:

* display IP major 9 and platform generation 9.5 separately;
* three display pipes, four CPU transcoders including EDP, and five possible
  logical DDIs;
* actual runtime/fused counts remain a laptop/VBT confirmation item.

The eDP timing read reports the **active firmware-left timing**, not a claim
that EDID was read and not a new mode. It is deliberately labelled `panel
unknown` when eDP is inactive, the power domain is unavailable, or the timing
fields are implausible.

## 4. EDID through AUX/DDC: the next, separate step

Reading EDID is useful, but it is not part of this read-only probe. Both AUX
and GMBUS reads require writes to controller registers, so they belong behind
an explicit later gate.

For the internal panel, the expected path is DisplayPort I2C-over-AUX on AUX A
(eDP/DDI A). On gen9 the AUX A control register is BAR0 `0x64010`, and its five
little-endian data words are `0x64014..0x64024`. Other channels have different
control banks; do not assume A for an external connector without VBT/DRM
mapping.

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
debugfs copy:

```sh
sudo cat /sys/kernel/debug/dri/0/i915_vbt > /tmp/x1c6-vbt.bin
sha256sum /tmp/x1c6-vbt.bin
```

Use the actual DRM minor number. If debugfs is unavailable, a read-only PCI
ROM/OpRegion dump is a fallback, but do not flash, unlock or write either ROM.
Record the source and hash of every blob.

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
reconnaissance material, not commands to replay.

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

## 8. Live-laptop confirmation checklist

The five facts that decide whether this probe can safely proceed are:

1. **PCI identity and decode:** exact BDF, `8086:5917`, revision, subsystem,
   class, BAR0 base/type/size, command and D0 state.
2. **Display topology:** i915 display version 9, runtime pipe/transcoder/port
   masks, fused-off pipes, and actual connector-to-DDI/AUX mapping.
3. **Panel identity:** eDP connector, EDID manufacturer/product/name, native
   resolution/timing and the currently active mode.
4. **VBT board data:** VBT/BDB version, panel type, child devices, panel name,
   DTD and color depth, with a SHA-256 of the captured blob.
5. **Power handshake:** whether firmware leaves the GPU in D0 and PW1/DDI state
   sufficient for read-only MMIO when Micro OS starts; no forcewake may be
   needed or acquired by this probe.

Read-only commands for the owner (run on the laptop; paths may use a DRM minor
other than 0):

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
sudo cat /sys/kernel/debug/dri/0/i915_vbt > /tmp/x1c6-vbt.bin
sha256sum /tmp/x1c6-vbt.bin
sudo dmesg | grep -Ei 'i915|drm|gpu|edid|vbt|dmc' | tail -80
```

Do not run `intel_reg_dumper`, `devmem`, EDID writers, VBIOS flashers or
forcewake debug tools for this reconnaissance.

## 9. Sources

* Lenovo PSREF, ThinkPad X1 Carbon 6th Gen platform specifications:
  <https://psref.lenovo.com/product/ThinkPad/ThinkPad_X1_Carbon_6th_Gen>
* Intel Graphics for Linux Programmer's Reference Manuals index (Kaby Lake
  volumes): <https://cdrdv2-public.intel.com/772631/graphics-for-linux_developer-reference_1.0-772629-772631.pdf>
* Linux v6.6 i915 PCI IDs:
  <https://github.com/torvalds/linux/blob/v6.6/include/drm/i915_pciids.h>
* Linux v6.6 display topology:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_display_device.c>
* Linux v6.6 register definitions:
  <https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/i915_reg.h>
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
