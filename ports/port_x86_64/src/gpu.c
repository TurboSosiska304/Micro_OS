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

static struct port_x86_64_gpu_info g_gpu;
static char g_gpu_line[160];
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

const char *port_x86_64_gpu_format(char *buf, k_size_t size)
{
    char vram[32];

    if (buf == NULL || size == 0u) {
        return "";
    }

    if (!g_gpu.found) {
        snprintf(buf, size,
                 "gpu: no display controller of a known vendor, the framebuffer is the firmware's");
        return buf;
    }

    format_size(vram, sizeof(vram), g_gpu.vram_bytes);
    snprintf(buf, size, "gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%u, mmio 0x%x, vram %s",
             g_gpu.name[0] != '\0' ? g_gpu.name : "display controller", (unsigned)g_gpu.vendor,
             (unsigned)g_gpu.device, (unsigned)g_gpu.revision, (unsigned)g_gpu.bus,
             (unsigned)g_gpu.slot, (unsigned)g_gpu.function, (unsigned)(k_u32_t)g_gpu.mmio_base,
             vram);
    return buf;
}

const struct port_x86_64_gpu_info *port_x86_64_gpu_info(void)
{
    return &g_gpu;
}

static bool probe_device(const struct port_x86_64_pci_device *pci)
{
    if (port_x86_64_gpu_amd_probe(pci, &g_gpu)) {
        return true;
    }

    return false;
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
                k_u16_t vendor = port_x86_64_pci_read16(&pci, PCI_VENDOR);
                k_u16_t class_rev;
                k_u8_t base;

                if (vendor == 0xFFFFu) {
                    continue;
                }

                class_rev = port_x86_64_pci_read16(&pci, PCI_CLASS);
                base = (k_u8_t)(class_rev >> 8);
                if (base != 0x03u) {
                    continue;
                }

                if (probe_device(&pci)) {
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
    return g_gpu.found ? 0 : 1;
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

    if (g_gpu_registered) {
        return;
    }

    g_gpu_registered = true;
    rc = kmod_register(&g_gpu_mod);
    if (rc != 0) {
        kerror("gpu: could not register kmod");
        return;
    }

    /* kmod_register sets state from init return: 0 OK, !=0 FAIL. We want SKIP
     * when no known GPU was found. Adjust after registration. */
    {
        k_size_t i;

        for (i = 0; i < kmod_count(); i++) {
            const struct kmod *m = kmod_get(i);
            if (m != NULL && strcmp(m->name, "gpu") == 0) {
                struct kmod *mw = (struct kmod *)m;
                if (!g_gpu.found) {
                    mw->state = KMOD_SKIP;
                    kinfo("kmod: gpu skip");
                } else {
                    kinfo("kmod: gpu ok");
                }
                break;
            }
        }
    }
}
