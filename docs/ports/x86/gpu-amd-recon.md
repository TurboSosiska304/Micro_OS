# AMD GPU recon — live hardware

* Date: 26 November 2026
* Machine: this desktop (CachyOS host), card present in the PCIe slot
* Card: `0000:2b:00.0` VGA compatible controller, Sapphire Nitro+ Radeon RX 570/580/590

This is the first step for an AMD GPU driver in Micro OS: identify the card,
read what can be read without firmware, and record what a bare-metal probe can
verify. No display control registers were written during this recon; every fact
below comes from reads of `lspci`, sysfs and the kernel log.

## 1. What we know about the card

| Fact | Value | Source |
|---|---|---|
| PCI BDF | `0000:2b:00.0` | `lspci -nn -s 2b:00.0` |
| Vendor:Device | `1002:67df` (AMD/ATI Ellesmere) | `lspci -nn`, sysfs `vendor`/`device` |
| PCI revision | `0xef` | sysfs `revision`, lspci |
| Subsystem | Sapphire Technology Limited Nitro+ Radeon RX 570/580/590 `[1da2:e366]` | lspci -vv |
| Class | `0300` VGA compatible controller, prog-if `00` | lspci |
| BAR0 | 64-bit prefetchable MMIO, `0x7c00000000`, size 8 GiB | lspci -vv, sysfs `resource` |
| BAR2 | 64-bit prefetchable MMIO, `0x7e00000000`, size 2 MiB | lspci -vv, sysfs `resource` |
| BAR4 | I/O ports, `0xe000`, size 256 | lspci -vv, sysfs `resource` |
| BAR5 | 32-bit non-prefetchable MMIO, `0xfce00000`, size 256 KiB (register MMIO) | lspci -vv, sysfs `resource`, amdgpu log |
| Expansion ROM | `0xfce40000`, 128 KiB, disabled | lspci -vv, sysfs `rom` size |
| PCI link | reported by lspci caps unavailable without sudo; kernel driver active | lspci -vv |
| Kernel driver (host) | `amdgpu` | lspci -vv, sysfs |
| VRAM (reported by amdgpu) | 8192 MiB GDDR5, 256-bit bus | kernel log |
| VRAM total (sysfs) | 8589934592 bytes | `/sys/class/drm/card1/device/mem_info_vram_total` |
| GTT (reported by amdgpu) | 11973 MiB | kernel log |
| Visible VRAM aperture | BAR0 is 8 GiB, matches VRAM size | lspci/sysfs |
| Register MMIO aperture | 256 KiB at BAR5 (`0xFCE00000`) | kernel log: "register mmio base: 0xFCE00000, size: 262144" |
| VBIOS | ATOM BIOS `113-2E366DU-S4U`, build 376739, ver `015.050.002.001.000000`, 2017/03/10 | kernel log |
| VBIOS source (host) | "Fetched VBIOS from VFCT" | kernel log |
| VBIOS ROM file | `/sys/bus/pci/devices/0000:2b:00.0/rom`, 131072 bytes, root-only read | sysfs |
| IP blocks (amdgpu discovery) | vi_common, gmc_v8_0, tonga_ih, gfx_v8_0, sdma_v3_0, powerplay/smu, dm/dce, uvd_v6_0, vce_v3_0 | kernel log |
| GPU topology | SE 4, SH/SE 1, CU/SH 9, active CU 32 | kernel log |
| Display core | DCE 11.2, Display Core v3.2.392 | kernel log |
| Firmware (loaded by amdgpu) | UVD 1.130 family 16, VCE 53.26 binary 3; others loaded by amdgpu (not enumerated without debugfs) | kernel log |

## 2. Recon commands and access limits

Commands run on the host without modifying device state:

* `lspci -vv -s 2b:00.0` — succeeded as unprivileged user; PCI capabilities were
  reported as `<access denied>`, but BARs, class, vendor/device, IRQ, IOMMU
  group and driver binding were visible.
* `lspci -nn -s 2b:00.0`, `lspci -t` — succeeded.
* `cat /sys/bus/pci/devices/0000:2b:00.0/{vendor,device,revision,resource}` — succeeded.
* `cat /sys/class/drm/card1/device/mem_info_vram_total` — succeeded (8589934592).
* `ls /sys/class/drm/card1/device/pp_dpm_*` — succeeded (mclk, pcie, sclk present).
* `journalctl -k -g amdgpu --no-pager` — succeeded; provided amdgpu discovery log.
* `dmesg | grep -i amdgpu` — **failed**: `read kernel buffer failed: Operation not permitted`.
* `sudo -n lspci -vv -s 2b:00.0` — **failed**: sudo password required.
* `sudo -n dmesg ...` — **failed**: sudo password required.
* `ls /sys/kernel/debug/dri/0/` — **failed**: Permission denied; debugfs register
  dumps (`amdgpu_regs`, `amdgpu_regs2`, `amdgpu_mc_info`, `amdgpu_gem_info`)
  were not readable.
* VBIOS ROM: `/sys/bus/pci/devices/0000:2b:00.0/rom` exists, 131072 bytes,
  mode `-rw------- root root`. Reading requires `echo 1 > rom` as root and then
  reading; not attempted because sudo was not available. VBIOS identity was
  obtained from the amdgpu kernel log instead.

## 3. What can be read without firmware

The constraint for the Micro OS driver is: no writes to display control
registers, no firmware loading, no ring setup, no mode set. Only reads that are
safe on a POSTed card with firmware already running (or registers that do not
depend on SMU/firmware).

### Safe to read in a bare-metal probe (high confidence)

| Item | How | Why safe |
|---|---|---|
| PCI config vendor/device/revision/class/subsystem | PCI config space reads (0x00,0x02,0x08,0x0A,0x2C) | Standard PCI config reads, no side effects |
| BAR addresses/sizes | PCI config BAR registers | Standard PCI enumeration; size probe uses write/read-back of BAR register itself — must be done carefully before MMIO use, but is normal PCI practice |
| PCI command register | PCI config 0x04 read | Read-only observation |
| MMIO BAR5 mapping presence | PCI BAR5 | MMIO aperture is fixed by PCI config |
| `mmMC_VM_FB_LOCATION` / `mmMC_VM_FB_OFFSET` family | BAR5 MMIO read | MC VM FB location registers on GCN/Polaris are readable and report VRAM aperture placement; amdgpu reads them during early init. Need exact register offsets for Polaris/gmc_v8_0. |
| `mmCONFIG_MEMSIZE` (0x5428 on many GCN) / VRAM size regs | BAR5 MMIO read | Commonly readable without firmware; confirms VRAM size. Exact Polaris offset must be validated against amdgpu headers. |
| GPU identity/version registers in mmio | BAR5 MMIO read | Chip revision/family registers in CONFIG/GC blocks are generally readable; exact offsets require amdgpu register definitions. |

### Needs care / not assumed safe without validation

| Item | Why |
|---|---|
| SMU-indexed registers (SMC_MSG, SMU_FW) | Require SMU mailbox protocol; SMU may not be initialized by Micro OS. Do not read through mailbox. |
| Display CRTC/plane registers | Read-only is probably harmless, but task forbids display control register writes; avoid touching display blocks entirely in this phase. |
| MC/SMU-dependent power/clock registers | May require firmware or specific sequencing. |
| Ring/MMIO doorbells beyond observation | Any write is out of scope. |

### Practical first-probe set for Micro OS

For the "hello GPU" phase, the safest minimal probe is:

1. PCI config: vendor `0x1002`, device, revision, class `0x03`, subsystem.
2. BAR discovery: BAR0 (VRAM aperture), BAR2 (doorbell/aperture), BAR5 (register MMIO).
3. Enable PCI memory decode (and bus master only if needed later; memory decode
   is enough for MMIO reads). Do **not** change display-related state.
4. Map BAR5 MMIO read-only/uncached.
5. Read a small set of identity registers known to be readable on Polaris without
   firmware. Candidate: `mmMC_VM_FB_LOCATION` (GMC), `mmCONFIG_MEMSIZE`, and
   `mmBIF_BX_PF0_GPU_HDP_FLUSH_REQ`? No - avoid HDP flush. Better use GC/CC
   version or MC size registers from amdgpu `gmc_v8_0`/`vi` headers. Exact offsets
   must be taken as hardware facts with provenance.

The current recon did not read BAR5 registers directly because that would
require mapping physical MMIO on the host or using debugfs; debugfs was not
accessible. The amdgpu kernel log confirms the card is POSTed, VRAM is 8 GiB,
and BAR5 is 256 KiB at `0xfce00000`.

## 4. What the bare-metal Micro OS probe should prove

When Micro OS boots on this desktop (separate manual test by owner):

1. PCI scan finds `1002:67df` rev `0xef` at bus `0x2b`, slot `0x00`, function `0`.
2. BAR0 is a 64-bit prefetchable memory window, 8 GiB.
3. BAR5 is a 32-bit non-prefetchable MMIO window, 256 KiB.
4. The driver logs a line like:
   `gpu: AMD Radeon RX 570 (1002:67df rev ef) at 2b:00.0, mmio 0xfce00000, vram 8192 MiB`
5. `kmod: gpu ok` (or `degraded` if MMIO identity reads are unavailable but PCI
   identity is good).
6. The firmware framebuffer remains untouched; no mode change, no flicker, no
   display corruption.

## 5. Provenance / rules

* No display control registers were written.
* No firmware was loaded.
* No ring was started.
* Facts above are from host read-only observation: `lspci`, sysfs, and
  `journalctl -k`. Linux amdgpu source was not copied; amdgpu kernel log was used
  only as observed runtime output on this machine.
* If amdgpu register offsets are later used in code, each file must carry a
  provenance note naming the upstream source path/revision and the hardware
  facts taken, per `docs/plan-gpu.md` section 5.
