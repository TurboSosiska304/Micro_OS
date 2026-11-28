/*
 * Micro OS — `install`, the disk installer of the x86-64 machine.
 *
 * The command reads the partition table of a disk, finds a gap large enough
 * for the requested size and prints the plan. A run without the exact
 * confirmation is a dry run and writes nothing. After the confirmation the
 * installer appends one entry, formats it, mounts it, writes the tree a boot
 * expects and copies the running system with a SHA-256 manifest (L3 and L4).
 * It then mounts the volume read-only and verifies every copied file before
 * saying "installed"; --verify repeats that check without writing anything.
 *
 * The safety contract is the one of the plan: one appended entry in a gap the
 * installer verified, no whole-disk format, no silent ESP rewrite, the other
 * OS stays bootable, and dry-run is the default. An error after the table
 * write is reported honestly: the entry stays, and removing it is a separate
 * decision of the person at the console.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#if defined(MICRO_OS_FS_EXT2) && MICRO_OS_FS_EXT2
#include "fs_ext2/ext2.h"
#endif
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/install.h"
#include "kernel/partition.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "port_x86_64.h"
#include "port_x86_64_install.h"
#include "port_x86_64_install_copy.h"

#define INSTALL_GIB (1024ULL * 1024ULL * 1024ULL)
#define INSTALL_MIB (1024ULL * 1024ULL)
#define INSTALL_POINT "/mnt/target"
#define INSTALL_LABEL "MICROOS_ROOT"
#define INSTALL_VOLUME_NAME "installvol"
#define INSTALL_MFS_RECORDS 256u

/* The type GUID of a Linux data partition, the one the appended entry uses. */
static const k_u8_t g_guid_linux[PARTITION_GUID_SIZE] = {
    0xAFu, 0x3Du, 0xC6u, 0x0Fu, 0x83u, 0x84u, 0x72u, 0x47u,
    0x8Eu, 0x79u, 0x3Du, 0x69u, 0xD8u, 0x47u, 0x7Du, 0xE4u,
};

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

/*
 * MicroFS asks for sectors of one kilobyte. A disk whose sectors are smaller
 * gets a view of the new partition at that grain; the view is registered only
 * for the install and leaves the table with it. Ext2 opens its own view when
 * it mounts, so it needs none here.
 */
struct install_volume {
    struct k_blockdev_view view;
    const char *name;
    bool opened;
};

static int install_volume_open(const struct k_blockdev *part, k_u32_t grain,
                               struct install_volume *out)
{
    memset(out, 0, sizeof(*out));
    out->name = part->name;

    if (part->sector_size == grain) {
        return 0;
    }

    if (k_blockdev_view_open(&out->view, part, grain) != 0) {
        return KERR_NOTSUP;
    }
    out->view.dev.name = INSTALL_VOLUME_NAME;
    if (k_blockdev_register(&out->view.dev) != 0) {
        k_blockdev_view_close(&out->view);
        return KERR_BUSY;
    }
    out->opened = true;
    out->name = out->view.dev.name;

    return 0;
}

static void install_volume_close(struct install_volume *out)
{
    if (out->opened) {
        (void)k_blockdev_unregister(&out->view.dev);
        k_blockdev_view_close(&out->view);
        out->opened = false;
    }
}

/* The entry the append made, found again after the table was re-read. */
static int find_new_partition(const struct k_blockdev *disk, struct partition_inventory *after,
                              k_u64_t first_lba, const char **name)
{
    k_u32_t i;

    for (i = 0; i < after->entry_count; i++) {
        const struct partition_entry_info *entry = &after->entries[i];

        if (entry->first_lba == first_lba && streq(entry->label, INSTALL_LABEL)) {
            *name = partition_name(disk, entry->index - 1u);
            return *name != NULL ? 0 : KERR_NOENT;
        }
    }

    return KERR_NOENT;
}

/*
 * The honest ending of a failure after the table was written. The GPT entry
 * is deliberately left in place: a silent rollback of a table a person
 * watched being written is a worse surprise than a partition to remove.
 */
static int install_after_gpt_error(const char *step, int rc, const char *disk_name)
{
    kshell_printf("install: %s failed (%d)\n", step, rc);
    kshell_printf("install: the GPT entry \"%s\" is still on %s; it was not removed\n",
                  INSTALL_LABEL, disk_name);
    kshell_printf("install: the volume may hold a formatted, partly copied tree; this is not an "
                  "installed, verified system\n");
    kshell_printf("install: inspect it with 'cat /proc/blockdev'; to start over, remove the "
                  "entry on purpose with a partitioning tool, then format again\n");

    return KSHELL_ERROR;
}

static int install_write(const struct k_blockdev *disk, struct partition_inventory *inv,
                         k_u64_t need_sectors, const char *fs, const struct partition_gap *gap)
{
    struct install_volume volume;
    struct install_record rec;
    const char *part_name = NULL;
    const struct k_blockdev *part;
    bool writes_open = false;
    bool mounted = false;
    k_u64_t start_lba = gap->first_lba; /* the plan's gap, before inv is re-read */
    k_u32_t files = 0u;
    int rc;

    /* The rule of ADR 0041 guards mounts. An install the operator confirmed
     * is the act that creates the volume, so this one time the table area and
     * the new partition are writable. */
    (void)port_x86_64_install_writes(disk, true);
    writes_open = true;

    rc = partition_append(disk, start_lba, need_sectors, g_guid_linux, INSTALL_LABEL);
    if (rc != 0) {
        (void)port_x86_64_install_writes(disk, false);
        kshell_printf("install: the GPT entry was not written (%d); the disk is unchanged\n", rc);
        return KSHELL_ERROR;
    }
    kshell_printf("install: GPT entry \"%s\" appended and verified\n", INSTALL_LABEL);

    rc = partition_scan(disk);
    if (rc < 0) {
        rc = KERR_IO;
        goto fail;
    }
    rc = partition_inventory(disk, inv);
    if (rc != 0) {
        goto fail;
    }
    rc = find_new_partition(disk, inv, start_lba, &part_name);
    if (rc != 0) {
        goto fail;
    }
    part = k_blockdev_find(part_name);
    if (part == NULL) {
        rc = KERR_NOENT;
        goto fail;
    }
    kshell_printf("install: new partition %s, %llu sector(s)\n", part_name,
                  (unsigned long long)need_sectors);

    rc = install_volume_open(part, streq(fs, "ext2") ? part->sector_size : 1024u, &volume);
    if (rc != 0) {
        goto fail;
    }

    if (streq(fs, "ext2")) {
#if defined(MICRO_OS_FS_EXT2) && MICRO_OS_FS_EXT2
        rc = ext2_format(part);
#else
        kshell_printf("install: this image has no ext2 support\n");
        rc = KERR_NOTSUP;
#endif
    } else {
        const struct k_blockdev *medium = volume.opened ? &volume.view.dev : part;

        rc = mfs_format(medium, INSTALL_MFS_RECORDS);
    }
    if (rc != 0) {
        install_volume_close(&volume);
        goto fail;
    }
    kshell_printf("install: %s filesystem created on %s\n", fs, volume.name);

    rc = vfs_mkdir_p(INSTALL_POINT);
    if (rc != 0 && rc != KERR_EXIST) {
        install_volume_close(&volume);
        goto fail;
    }
    rc = vfs_mount_type(INSTALL_POINT, streq(fs, "ext2") ? "ext2" : "mfs", volume.name);
    if (rc != 0) {
        install_volume_close(&volume);
        goto fail;
    }
    mounted = true;

    rec.volume = part_name;
    rec.fs = fs;
    rec.boot_mode = "uefi";
    rec.start_lba = start_lba;
    rec.sector_count = need_sectors;
    rc = install_tree_create(INSTALL_POINT, &rec);
    if (rc != 0) {
        goto fail;
    }
    kshell_printf("install: tree written: /etc/volumes, /etc/install, /dev, /proc, /tmp, /mnt\n");

    rc = port_x86_64_install_copy(INSTALL_POINT, &files);
    if (rc != 0) {
        goto fail;
    }
    kshell_printf("install: copied %u file(s): the kernel module, /bin and /etc defaults\n",
                  (unsigned)files);

    rc = vfs_umount(INSTALL_POINT);
    mounted = false;
    if (rc != 0) {
        goto fail;
    }

    (void)port_x86_64_install_writes(disk, false);
    writes_open = false;

    /*
     * The second mount asks for no writes: the bytes are read back through the
     * same VFS, but nothing in this pass can change the volume. The check is
     * the gate; only after it does the command say "installed".
     */
    rc = vfs_mount_type(INSTALL_POINT, streq(fs, "ext2") ? "ext2" : "mfs", volume.name);
    if (rc != 0) {
        install_volume_close(&volume);
        return install_after_gpt_error("the read-only mount", rc, disk->name);
    }
    mounted = true;

    rc = port_x86_64_install_verify(INSTALL_POINT, &files);
    (void)vfs_umount(INSTALL_POINT);
    mounted = false;
    install_volume_close(&volume);

    if (rc != 0) {
        return install_after_gpt_error("verification", rc, disk->name);
    }

    kshell_printf("installed, verified: %u file(s) on %s; boot files and NVRAM were not touched\n",
                  (unsigned)files, part_name);

    return KSHELL_OK;

fail:
    if (mounted) {
        (void)vfs_umount(INSTALL_POINT);
    }
    install_volume_close(&volume);
    if (writes_open) {
        (void)port_x86_64_install_writes(disk, false);
    }

    return install_after_gpt_error("install", rc, disk->name);
}

/* The entry L4 appended, found again by its label for a later check. */
static int find_installed_partition(const struct k_blockdev *disk,
                                    const struct partition_inventory *inv, const char **name)
{
    k_u32_t i;

    for (i = 0u; i < inv->entry_count; i++) {
        if (streq(inv->entries[i].label, INSTALL_LABEL)) {
            *name = partition_name(disk, inv->entries[i].index - 1u);
            return *name != NULL ? 0 : KERR_NOENT;
        }
    }

    return KERR_NOENT;
}

/* A second, read-only pass over a volume that was installed earlier. It opens
 * no write door and appends nothing: a failed check changes no bytes. */
static int install_verify_volume(const struct k_blockdev *disk, const char *fs,
                                 const char *part_name)
{
    struct install_volume volume;
    const struct k_blockdev *part = k_blockdev_find(part_name);
    k_u32_t files = 0u;
    bool mounted = false;
    int rc;

    if (part == NULL) {
        kshell_printf("install: %s has no partition %s\n", disk->name, part_name);
        return KSHELL_ERROR;
    }

    rc = install_volume_open(part, streq(fs, "mfs") ? 1024u : part->sector_size, &volume);
    if (rc != 0) {
        kshell_printf("install: %s: cannot open %s for verification (%d)\n", disk->name, part_name,
                      rc);
        return KSHELL_ERROR;
    }

    rc = vfs_mkdir_p(INSTALL_POINT);
    if (rc == KERR_EXIST) {
        rc = 0;
    }
    if (rc == 0) {
        rc = vfs_mount_type(INSTALL_POINT, streq(fs, "ext2") ? "ext2" : "mfs", volume.name);
        if (rc == 0) {
            mounted = true;
            rc = port_x86_64_install_verify(INSTALL_POINT, &files);
        }
    }
    if (mounted) {
        (void)vfs_umount(INSTALL_POINT);
    }
    install_volume_close(&volume);

    if (rc != 0) {
        kshell_printf("install: verification failed (%d); the volume is left as it is\n", rc);
        return KSHELL_ERROR;
    }

    kshell_printf("installed, verified: %u file(s) on %s\n", (unsigned)files, part_name);
    return KSHELL_OK;
}

static int install_main(int argc, char **argv)
{
    const char *disk_name = NULL;
    const char *size_text = NULL;
    const char *fs = NULL;
    const char *confirm = NULL;
    const char *part_name = NULL;
    bool verify_mode = false;
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
        } else if (streq(argv[i], "--verify")) {
            verify_mode = true;
        } else if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            kshell_printf("usage: install --disk NAME --size N{G|M} --fs {ext2|mfs} [--confirm "
                          "\"INSTALL NAME SIZE\"]\n");
            kshell_printf("       install --disk NAME --fs {ext2|mfs} --verify\n");
            kshell_printf("  dry-run by default: prints the plan, writes nothing\n");
            return KSHELL_OK;
        } else {
            kshell_printf("install: unknown argument: %s\n", argv[i]);
            return KSHELL_USAGE;
        }
    }

    if (fs == NULL) {
        kshell_printf("install: --fs {ext2|mfs} is required (no silent default)\n");
        return KSHELL_USAGE;
    }

    if (!streq(fs, "ext2") && !streq(fs, "mfs")) {
        kshell_printf("install: unsupported --fs: %s (use ext2 or mfs)\n", fs);
        return KSHELL_USAGE;
    }

    if (verify_mode) {
        if (size_text != NULL || confirm != NULL || disk_name == NULL) {
            kshell_printf("install: --verify takes --disk NAME and --fs only\n");
            return KSHELL_USAGE;
        }

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
        if (rc != 0) {
            kshell_printf("install: cannot read the partition table of %s (%d)\n", disk_name, rc);
            return KSHELL_ERROR;
        }

        rc = find_installed_partition(disk, &inv, &part_name);
        if (rc != 0) {
            kshell_printf("install: no \"%s\" entry on %s\n", INSTALL_LABEL, disk_name);
            return KSHELL_ERROR;
        }

        return install_verify_volume(disk, fs, part_name);
    }

    if (disk_name == NULL || size_text == NULL) {
        kshell_printf("install: --disk and --size are required\n");
        return KSHELL_USAGE;
    }

    if (!parse_size(size_text, &size_bytes, size_label, sizeof(size_label))) {
        kshell_printf("install: bad size: %s (use N{G|M})\n", size_text);
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

    kshell_printf("action: append entry \"%s\" (Linux data GUID) of %s,\n", INSTALL_LABEL,
                  size_label);
    kshell_printf("        format as %s, mount as /, install Micro OS 2.4\n", fs);

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

    return install_write(disk, &inv, need_sectors, fs, gap);
}

int port_x86_64_install_writes(const struct k_blockdev *disk, bool open)
{
    if (disk != NULL && disk->name != NULL) {
        if (strcmp(disk->name, "nvme0") == 0) {
            nvme_install_writes(open);
            return 0;
        }
        if (strcmp(disk->name, "ahci0") == 0) {
            ahci_install_writes(open);
            return 0;
        }
    }

    return k_blockdev_gated(disk) ? KERR_NOTSUP : 0;
}

int port_x86_64_install_register(void)
{
    static const struct kshell_help help = {
        .summary = "install Micro OS to a disk (dry-run by default)",
        .usage = "install --disk NAME --size N{G|M} --fs {ext2|mfs} "
                 "[--confirm \"INSTALL NAME SIZE\"] | install --disk NAME --fs {ext2|mfs} --verify",
        .detail = "  --disk NAME     target disk (whole disk, not a partition)\n"
                  "  --size N{G|M}   size of the partition to create\n"
                  "  --fs ext2|mfs   filesystem (required, no silent default)\n"
                  "  --confirm STR   exact confirmation; with it: append, format, copy the\n"
                  "                  system, then mount read-only and verify it\n"
                  "  --verify        check the copy of an earlier install, read-only\n"
                  "  --dry-run       print the plan, write nothing (default)\n",
        .owns_short_h = false,
    };

    return kshell_register_help("install", &help, install_main);
}
