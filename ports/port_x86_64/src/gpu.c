/*
 * Micro OS — the x86-64 port: display controller discovery ("hello GPU").
 *
 * This is the first step of a GPU driver: find a display controller on PCI,
 * ask a vendor probe to identify it by read-only means, and report what
 * answered. It does not change display mode, does not write display engine
 * registers, does not load firmware, and does not start rings.
 *
 * Provenance: original implementation for Micro OS. Hardware facts were
 * observed on a live Sapphire Nitro+ RX 570 (1002:67df rev ef) on 26 Nov 2026
 * using lspci/sysfs/journalctl; no Linux source code was copied. See
 * docs/ports/x86/gpu-amd-recon.md and docs/plan-gpu.md section 5.
 */
#include "port_x86_64_gpu.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kshell/kshell.h"

static struct gpu_info g_gpu;
static char g_gpu_line[256];
static bool g_gpu_registered;

static void format_size(char *buf, k_size_t size, k_u64_t bytes)
{
    if (bytes == 0u) {
        snprintf(buf, size, "unknown");
        return;
    }

    if (bytes % (1024ull * 1024ull * 1024ull) == 0u) {
        snprintf(buf, size, "%u GiB", (unsigned)(bytes / (1024ull * 1024ull * 1024ull)));
    } else if (bytes % (1024ull * 1024ull) == 0u) {
        snprintf(buf, size, "%u MiB", (unsigned)(bytes / (1024ull * 1024ull)));
    } else {
        snprintf(buf, size, "%u KiB", (unsigned)(bytes / 1024ull));
    }
}

static void format_mmio(char *buf, k_size_t size, k_u64_t base, k_u32_t bytes)
{
    char base_buf[32];
    const char *suffix = "B";
    unsigned value = bytes;

    if (base == 0u || bytes == 0u) {
        snprintf(buf, size, "unknown");
        return;
    }

    if ((bytes % (1024u * 1024u)) == 0u) {
        suffix = "M";
        value = bytes / (1024u * 1024u);
    } else if ((bytes % 1024u) == 0u) {
        suffix = "K";
        value = bytes / 1024u;
    }

    if ((base >> 32) != 0u) {
        snprintf(base_buf, sizeof(base_buf), "0x%x%08x", (unsigned)(base >> 32),
                 (unsigned)(k_u32_t)base);
    } else {
        snprintf(base_buf, sizeof(base_buf), "0x%x", (unsigned)(k_u32_t)base);
    }

    snprintf(buf, size, "%s+%u%s", base_buf, value, suffix);
}

static void format_tail(char *buf, k_size_t size, const struct gpu_info *info)
{
    char vram[32];

    if (info->display_gen != 0u) {
        const char *step = info->display_step[0] != '\0' ? info->display_step : "unknown";

        if (info->panel_width != 0u && info->panel_height != 0u) {
            snprintf(buf, size, "display %u.%02u stepping %s, panel %ux%u",
                     (unsigned)info->display_gen, (unsigned)info->display_rel, step,
                     (unsigned)info->panel_width, (unsigned)info->panel_height);
        } else {
            snprintf(buf, size, "display %u.%02u stepping %s, panel unknown",
                     (unsigned)info->display_gen, (unsigned)info->display_rel, step);
        }
        return;
    }

    if (info->vram_mc_bytes != 0u) {
        unsigned mc_mib = (unsigned)(info->vram_mc_bytes / (1024ull * 1024ull));
        if (info->gc_num_se != 0u) {
            snprintf(buf, size, "vram %u MiB (MC), mc chmap 0x%08x l1tlb 0x%08x, se %u sh/se %u",
                     mc_mib, (unsigned)info->mc_shared_chmap, (unsigned)info->mc_vm_mx_l1_tlb_cntl,
                     (unsigned)info->gc_num_se, (unsigned)info->gc_num_sh_per_se);
        } else {
            snprintf(buf, size, "vram %u MiB (MC), mc chmap 0x%08x l1tlb 0x%08x", mc_mib,
                     (unsigned)info->mc_shared_chmap, (unsigned)info->mc_vm_mx_l1_tlb_cntl);
        }
        return;
    }

    format_size(vram, sizeof(vram), info->vram_bytes);
    snprintf(buf, size, "vram %s", vram);
}

const char *port_x86_64_gpu_format(char *buf, k_size_t size)
{
    char mmio[32];
    char tail[128];
    const char *name;

    if (buf == NULL || size == 0u) {
        return "";
    }

    if (!g_gpu.present) {
        snprintf(buf, size,
                 "gpu: no display controller of a known vendor, the framebuffer is the firmware's");
        return buf;
    }

    format_mmio(mmio, sizeof(mmio), g_gpu.mmio_base, g_gpu.mmio_size);
    format_tail(tail, sizeof(tail), &g_gpu);

    if (!g_gpu.known) {
        if (g_gpu.vendor == 0x1002u) {
            name = "AMD display controller";
        } else if (g_gpu.vendor == 0x8086u) {
            name = "Intel display controller";
        } else {
            name = "display controller";
        }
    } else if (g_gpu.name[0] != '\0') {
        name = g_gpu.name;
    } else {
        name = "display controller";
    }

    snprintf(buf, size, "gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%u, mmio %s, %s", name,
             (unsigned)g_gpu.vendor, (unsigned)g_gpu.device, (unsigned)g_gpu.revision,
             (unsigned)g_gpu.bus, (unsigned)g_gpu.slot, (unsigned)g_gpu.function, mmio, tail);
    return buf;
}

const struct gpu_info *port_x86_64_gpu_info(void)
{
    return &g_gpu;
}

static enum gpu_probe_result probe_vendor(const struct port_x86_64_pci_device *pci, k_u16_t vendor,
                                          struct gpu_info *out)
{
    k_size_t i;

    for (i = 0; i < PORT_X86_64_GPU_PROBE_COUNT; i++) {
        if (PORT_X86_64_GPU_PROBES[i].vendor != vendor) {
            continue;
        }

        if (PORT_X86_64_GPU_PROBES[i].probe == NULL) {
            return GPU_PROBE_UNKNOWN_DEV;
        }

        return PORT_X86_64_GPU_PROBES[i].probe(pci, out);
    }

    return GPU_PROBE_NONE;
}

static bool is_display_controller(const struct port_x86_64_pci_device *pci)
{
    k_u16_t class_rev = port_x86_64_pci_read16(pci, PCI_CLASS);

    return (k_u8_t)(class_rev >> 8) == 0x03u;
}

static int gpu_mod_init(void)
{
    struct port_x86_64_pci_device pci;
    k_u16_t bus;

    memset(&g_gpu, 0, sizeof(g_gpu));

    for (bus = 0; bus < 256u; bus++) {
        pci.bus = (k_u8_t)bus;
        for (pci.slot = 0; pci.slot < 32u; pci.slot++) {
            for (pci.function = 0; pci.function < 8u; pci.function++) {
                k_u16_t vendor;
                enum gpu_probe_result res;
                struct gpu_info candidate;

                vendor = port_x86_64_pci_read16(&pci, PCI_VENDOR);
                if (vendor == 0xFFFFu) {
                    continue;
                }

                if (!is_display_controller(&pci)) {
                    continue;
                }

                memset(&candidate, 0, sizeof(candidate));
                candidate.present = true;
                candidate.vendor = vendor;
                candidate.device = port_x86_64_pci_read16(&pci, PCI_DEVICE);
                candidate.revision = (k_u8_t)(port_x86_64_pci_read16(&pci, PCI_REVISION) & 0xFFu);
                candidate.bus = pci.bus;
                candidate.slot = pci.slot;
                candidate.function = pci.function;

                res = probe_vendor(&pci, vendor, &candidate);
                if (res == GPU_PROBE_OK) {
                    candidate.present = true;
                    candidate.known = true;
                    candidate.probe_result = res;
                    g_gpu = candidate;
                    port_x86_64_gpu_format(g_gpu_line, sizeof(g_gpu_line));
                    kinfo("%s", g_gpu_line);
                    return 0;
                }

                if (res == GPU_PROBE_UNKNOWN_DEV || res == GPU_PROBE_DEGRADED ||
                    res == GPU_PROBE_FAIL) {
                    candidate.present = true;
                    candidate.known = (res != GPU_PROBE_UNKNOWN_DEV);
                    candidate.probe_result = res;
                    if (candidate.name[0] == '\0') {
                        snprintf(candidate.name, sizeof(candidate.name), "display controller");
                    }
                    g_gpu = candidate;
                    port_x86_64_gpu_format(g_gpu_line, sizeof(g_gpu_line));
                    kinfo("%s", g_gpu_line);
                    return 0;
                }
            }
        }
    }

    port_x86_64_gpu_format(g_gpu_line, sizeof(g_gpu_line));
    kinfo("%s", g_gpu_line);
    return 0;
}

static const char *gpu_mod_detail(void)
{
    return g_gpu_line[0] != '\0' ? g_gpu_line : "not probed";
}

static const struct kmod g_gpu_mod = {
    .name = "gpu",
    .desc = "display controller probe",
    .init = gpu_mod_init,
    .check = NULL,
    .detail = gpu_mod_detail,
};

static int cmd_gpu(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    port_x86_64_gpu_format(g_gpu_line, sizeof(g_gpu_line));
    kshell_printf("%s\n", g_gpu_line);
    return g_gpu.present ? 0 : 1;
}

int port_x86_64_gpu_register(void)
{
    int rc;

    rc = kshell_register("gpu", "display controller probe", cmd_gpu);
    if (rc == 0) {
        kinfo("gpu: command registered");
    }
    return rc;
}

void port_x86_64_gpu_init(void)
{
    int rc;
    k_size_t i;

    if (g_gpu_registered) {
        return;
    }

    g_gpu_registered = true;
    rc = kmod_register(&g_gpu_mod);
    if (rc != 0) {
        kerror("gpu: could not register kmod");
        return;
    }

    for (i = 0; i < kmod_count(); i++) {
        const struct kmod *m = kmod_get(i);
        if (m != NULL && strcmp(m->name, "gpu") == 0) {
            struct kmod *mw = (struct kmod *)m;

            if (g_gpu.probe_result == GPU_PROBE_OK) {
                mw->state = KMOD_OK;
                kinfo("kmod: gpu ok");
            } else if (g_gpu.probe_result == GPU_PROBE_UNKNOWN_DEV) {
                mw->state = KMOD_UNKNOWN;
                kinfo("kmod: gpu unknown");
            } else if (g_gpu.probe_result == GPU_PROBE_DEGRADED) {
                mw->state = KMOD_DEGRADED;
                kinfo("kmod: gpu degraded");
            } else if (g_gpu.probe_result == GPU_PROBE_FAIL) {
                mw->state = KMOD_FAIL;
                kinfo("kmod: gpu fail");
            } else {
                mw->state = KMOD_SKIP;
                kinfo("kmod: gpu skip");
            }
            break;
        }
    }
}
