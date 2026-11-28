/*
 * Micro OS — `install`, the dry-run skeleton of the disk installer.
 *
 * The command reads the partition table of a disk, finds a gap large enough
 * for the requested size, and prints the plan. It writes nothing: every run
 * without an explicit confirmation is a dry-run, and in this lane even a
 * confirmed run refuses with "not implemented" until the later lanes land.
 *
 * The safety contract is the one of docs/plan-install.md: one appended entry
 * in a gap the installer verified, no whole-disk format, no silent ESP
 * rewrite, the other OS stays bootable, and dry-run is the default.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/partition.h"
#include "kernel/vfs.h"
#include "port_x86_64.h"

#define INSTALL_GIB (1024ULL * 1024ULL * 1024ULL)
#define INSTALL_MIB (1024ULL * 1024ULL)

static bool streq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static bool parse_size(const char *text, k_u64_t *out_bytes, char *label, size_t label_size)
{
    k_u64_t value = 0;
    size_t i = 0;
    char suffix;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10u + (k_u64_t)(text[i] - '0');
        i++;
    }

    if (i == 0 || value == 0) {
        return false;
    }

    suffix = text[i];
    if (suffix == 'G' || suffix == 'g') {
        *out_bytes = value * INSTALL_GIB;
        (void)snprintf(label, label_size, "%lluG", (unsigned long long)value);
        i++;
    } else if (suffix == 'M' || suffix == 'm') {
        *out_bytes = value * INSTALL_MIB;
        (void)snprintf(label, label_size, "%lluM", (unsigned long long)value);
        i++;
    } else {
        return false;
    }

    return text[i] == '\0';
}

static bool disk_is_busy(const struct k_blockdev *disk)
{
    k_size_t i;
    k_size_t count = vfs_mount_count();
    k_size_t p;

    for (i = 0; i < count; i++) {
        const char *dev = vfs_mount_device(i);

        if (dev == NULL) {
            continue;
        }

        if (strcmp(dev, disk->name) == 0) {
            return true;
        }

        for (p = 0; p < K_PARTITION_MAX; p++) {
            const char *pname = partition_name(disk, p);

            if (pname == NULL) {
                continue;
            }

            if (strcmp(dev, pname) == 0) {
                return true;
            }
        }
    }

    return false;
}

static const struct partition_entry_info *find_esp(const struct partition_inventory *inv)
{
    k_u32_t i;

    for (i = 0; i < inv->entry_count; i++) {
        if ((inv->entries[i].flags & PARTITION_FLAG_ESP) != 0u) {
            return &inv->entries[i];
        }
    }

    return NULL;
}

static const struct partition_gap *find_gap(const struct partition_inventory *inv,
                                            k_u64_t need_sectors)
{
    k_u32_t i;
    const struct partition_gap *best = NULL;

    for (i = 0; i < inv->gap_count; i++) {
        const struct partition_gap *g = &inv->gaps[i];

        if (g->sectors < need_sectors) {
            continue;
        }

        if (best == NULL || g->sectors < best->sectors) {
            best = g;
        }
    }

    return best;
}

static void print_size_gib(k_u64_t sectors)
{
    k_u64_t bytes = sectors * 512u;
    k_u64_t gib10 = (bytes * 10u) / INSTALL_GIB;

    kshell_printf("%llu.%llu GiB", (unsigned long long)(gib10 / 10u),
                  (unsigned long long)(gib10 % 10u));
}

static int install_main(int argc, char **argv)
{
    const char *disk_name = NULL;
    const char *size_text = NULL;
    const char *fs = NULL;
    const char *confirm = NULL;
    bool dry_run = true;
    k_u64_t size_bytes = 0;
    char size_label[32];
    k_u64_t need_sectors;
    const struct k_blockdev *disk;
    struct partition_inventory inv;
    const struct partition_entry_info *esp;
    const struct partition_gap *gap;
    char confirm_want[96];
    int rc;
    int i;

    size_label[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--disk") && i + 1 < argc) {
            disk_name = argv[++i];
        } else if (streq(argv[i], "--size") && i + 1 < argc) {
            size_text = argv[++i];
        } else if (streq(argv[i], "--fs") && i + 1 < argc) {
            fs = argv[++i];
        } else if (streq(argv[i], "--confirm") && i + 1 < argc) {
            confirm = argv[++i];
            dry_run = false;
        } else if (streq(argv[i], "--dry-run")) {
            dry_run = true;
        } else if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            kshell_printf("usage: install --disk NAME --size N{G|M} --fs {ext2|mfs} [--confirm "
                          "\"INSTALL NAME SIZE\"]\n");
            kshell_printf("  dry-run by default: prints the plan, writes nothing\n");
            return KSHELL_OK;
        } else {
            kshell_printf("install: unknown argument: %s\n", argv[i]);
            return KSHELL_USAGE;
        }
    }

    if (disk_name == NULL || size_text == NULL) {
        kshell_printf("install: --disk and --size are required\n");
        return KSHELL_USAGE;
    }

    if (!parse_size(size_text, &size_bytes, size_label, sizeof(size_label))) {
        kshell_printf("install: bad size: %s (use N{G|M})\n", size_text);
        return KSHELL_USAGE;
    }

    if (fs == NULL) {
        kshell_printf("install: --fs {ext2|mfs} is required (no silent default)\n");
        return KSHELL_USAGE;
    }

    if (!streq(fs, "ext2") && !streq(fs, "mfs")) {
        kshell_printf("install: unsupported --fs: %s (use ext2 or mfs)\n", fs);
        return KSHELL_USAGE;
    }

    (void)snprintf(confirm_want, sizeof(confirm_want), "INSTALL %s %s", disk_name, size_label);

    disk = k_blockdev_find(disk_name);
    if (disk == NULL) {
        kshell_printf("install: no such disk: %s\n", disk_name);
        return KSHELL_ERROR;
    }

    if (disk_is_busy(disk)) {
        kshell_printf("install: %s is busy (live medium or mounted)\n", disk_name);
        return KSHELL_ERROR;
    }

    rc = partition_inventory(disk, &inv);
    if (rc == KERR_IO) {
        kshell_printf("install: GPT is broken, has no backup, or MBR is hybrid: %s (error %d)\n",
                      disk_name, rc);
        return KSHELL_ERROR;
    }
    if (rc != 0) {
        kshell_printf("install: cannot read partition table of %s (error %d)\n", disk_name, rc);
        return KSHELL_ERROR;
    }

    esp = find_esp(&inv);
    if (esp == NULL) {
        kshell_printf("install: no ESP on %s\n", disk_name);
        return KSHELL_ERROR;
    }

    need_sectors = (size_bytes + 511u) / 512u;
    gap = find_gap(&inv, need_sectors);
    if (gap == NULL) {
        kshell_printf("install: no gap large enough for %s on %s\n", size_label, disk_name);
        return KSHELL_ERROR;
    }

    /* The plan: nothing is written. */
    kshell_printf("disk:   %s (", disk_name);
    print_size_gib(inv.disk_lbas);
    kshell_printf(", GPT, %u entries, ESP p%u ", (unsigned)inv.entry_count, (unsigned)esp->index);
    print_size_gib(esp->sectors);
    kshell_printf(")\n");

    kshell_printf("space:  gap at sector %llu, ", (unsigned long long)gap->first_lba);
    print_size_gib(gap->sectors);
    kshell_printf(" free, aligned, unclaimed by any entry\n");

    kshell_printf("action: append entry \"MICROOS_ROOT\" (Linux data GUID) of %s,\n", size_label);
    kshell_printf("        format as %s, mount as /, install Micro OS 2.3\n", fs);

    kshell_printf("boot:   copy \\EFI\\MICROOS\\BOOTX64.EFI to ESP p%u; the NVRAM entry is asked\n",
                  (unsigned)esp->index);
    kshell_printf("        after the copy\n");
    kshell_printf("source: /boot/kernel, /boot/loader, /bin, /etc, /lib if present\n");
    kshell_printf("risk:   %s of unpartitioned space becomes a partition; ESP p%u gains\n",
                  size_label, (unsigned)esp->index);
    kshell_printf("        \\EFI\\MICROOS\\; no existing entry is modified\n");

    if (confirm == NULL || dry_run) {
        kshell_printf("plan:   dry-run, nothing written (pass --confirm \"%s\" to proceed)\n",
                      confirm_want);
        return KSHELL_OK;
    }

    if (!streq(confirm, confirm_want)) {
        kshell_printf("install: confirmation must be exactly: %s\n", confirm_want);
        return KSHELL_ERROR;
    }

    kshell_printf("install: not implemented (this lane is dry-run only)\n");
    return KSHELL_ERROR;
}

int port_x86_64_install_register(void)
{
    static const struct kshell_help help = {
        .summary = "install Micro OS to a disk (dry-run skeleton)",
        .usage =
            "install --disk NAME --size N{G|M} --fs {ext2|mfs} [--confirm \"INSTALL NAME SIZE\"]",
        .detail = "  --disk NAME     target disk (whole disk, not a partition)\n"
                  "  --size N{G|M}   size of the partition to create\n"
                  "  --fs ext2|mfs   filesystem (required, no silent default)\n"
                  "  --confirm STR   exact confirmation; at this stage: not implemented\n"
                  "  --dry-run       print the plan, write nothing (default)\n",
        .owns_short_h = false,
    };

    return kshell_register_help("install", &help, install_main);
}
