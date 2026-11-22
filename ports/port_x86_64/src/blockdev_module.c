/*
 * Micro OS — the x86-64 port: the volume the boot came from.
 *
 * A boot loader hands the kernel the image it runs and, after it, the files
 * of the boot that the loader was told about. On this port the second module
 * is a whole MicroFS disk: the root image of the ISO (ADR 0026), with /bin and
 * /etc already written by tools/mkfs_mfs.c. The port does not copy it — it is
 * eight megabytes, and the machine that boots an ISO has no disk to copy it
 * to — so the bytes of the module are the medium, read where the loader left
 * them.
 *
 * The medium is read-only on purpose. What a boot loader puts in the memory
 * of the machine is not a place to keep a file: the bytes are part of the
 * image, a second boot reads them again, and a write would be lost with the
 * machine and could break the only copy of /bin in the meantime. So the
 * device answers "no" to a write and the filesystem above it reports the
 * error to the caller, which is the honest answer of a machine whose root is
 * a CD.
 *
 * The device is one block per sector of MicroFS and its first partition is
 * the volume, exactly the shape of the disk of ADR 0008: the root image is
 * written by the same tool as programs.img, and the same parser reads its
 * table. A machine with a disk and an ISO in it mounts the ISO: the root of a
 * machine is the medium it was started from.
 */
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/partition.h"
#include "mfs/mfs.h"

#include "port_x86_64.h"

/* The kernel is module 0 of the boot; the root image is the one after it. */
#define MODULE_INDEX 1u

/*
 * The identity map of the loader covers the first gigabyte, and a module past
 * it could not be read at all. A loader with more memory than that would
 * still place a module of eight megabytes below it; a module that is not
 * there is refused instead of read through a missing page.
 */
#define IDENTITY_LIMIT 0x40000000ull

/* The smallest module that can hold a partition table and a volume: the MBR
 * and the first megabyte of the tool are not a volume, and anything shorter
 * is not a root image. */
#define MODULE_MIN_BYTES (2u * MFS_BLOCK_SIZE)

static const k_u8_t *g_module_base;
static k_u32_t g_module_sectors;
static const char *g_module_root;

/* A request past the end of the module is refused, not shortened: the
 * filesystem above has to know that it did not read the block it asked for. */
static bool module_range_ok(k_u32_t sector, k_size_t count)
{
    if (count == 0 || sector >= g_module_sectors) {
        return false;
    }

    return count <= (k_size_t)g_module_sectors - sector;
}

static int module_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || g_module_base == NULL || !module_range_ok(sector, count)) {
        return -1;
    }

    memcpy(buf, g_module_base + (k_size_t)sector * MFS_BLOCK_SIZE, count * MFS_BLOCK_SIZE);

    return 0;
}

/*
 * The bytes of the module are the image of the boot, not a medium of the
 * machine: a write would change the copy only until the next boot and could
 * leave the volume half written in the meantime.
 */
static int module_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(sector);
    K_UNUSED(buf);
    K_UNUSED(count);

    return -1;
}

/*
 * There is nothing to bring to the medium: the module is the image of the
 * boot and the bytes are already where the next boot will find them. The
 * call exists because the partition above a device forwards every operation
 * of the block layer to it, including this one, and a missing one would be a
 * jump through a null pointer the first time the filesystem closes a file.
 */
static int module_sync(void *ctx)
{
    K_UNUSED(ctx);

    return 0;
}

static const struct k_blockdev_ops g_module_ops = {
    .read = module_read,
    .write = module_write,
    .erase = NULL, /* there is nothing to erase: the medium is the image */
    .sync = module_sync,
};

static struct k_blockdev g_module_device = {
    .name = "boot0",
    .sector_size = MFS_BLOCK_SIZE,
    .sectors = 0, /* the size of the module, set by the init below */
    .ops = &g_module_ops,
    .ctx = NULL, /* the module is the bytes of the boot information */
};

void port_x86_64_module_init(void)
{
    k_u64_t start = 0;
    k_u64_t size = 0;
    const char *first;
    int partitions;

    if (!port_x86_64_boot_module(MODULE_INDEX, &start, &size)) {
        /* The machine of `make x86-run` boots with a disk or with ram0 and
         * has no second module; that is a boot, not a failure. */
        return;
    }

    if (start + size > IDENTITY_LIMIT) {
        kwarn("x86-64: the root image of the boot is past the identity map, not used");
        return;
    }

    if (size < MODULE_MIN_BYTES) {
        kwarn("x86-64: the module %u of the boot is %u byte(s), not a root image", MODULE_INDEX,
              (unsigned)size);
        return;
    }

    g_module_base = (const k_u8_t *)(uintptr_t)start;
    g_module_sectors = (k_u32_t)(size / MFS_BLOCK_SIZE);
    g_module_device.sectors = g_module_sectors;

    if (k_blockdev_register(&g_module_device) != 0) {
        kerror("x86-64: cannot register the volume of the boot");
        g_module_base = NULL;
        return;
    }

    kinfo("x86-64: the boot carries a read-only volume of %u KB (module %u)",
          (unsigned)(size / 1024), MODULE_INDEX);

    /* The root image is the disk of `make x86-disk`: an MBR and a MicroFS
     * volume in its first partition. The parser of the kernel reads the
     * table and registers the child; the boot mounts that child. */
    partitions = partition_scan(&g_module_device);

    if (partitions == KERR_NOENT) {
        kinfo("boot0: no partition table, the volume is not used");
        return;
    }

    if (partitions < 0) {
        kerror("boot0: the partition table is not usable (%d)", partitions);
        return;
    }

    first = partition_name(&g_module_device, 0);

    if (first == NULL) {
        return;
    }

    g_module_root = first;
    kinfo("x86-64: the root volume is %s (%d partition(s) of the boot medium)", first, partitions);
}

const char *port_x86_64_module_root(void)
{
    return g_module_root;
}
