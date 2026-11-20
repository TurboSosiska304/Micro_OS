/*
 * Micro OS — the x86-64 port: a volume in the memory of the machine.
 *
 * A filesystem is a consumer of blocks (kernel/blockdev.h), and this port has
 * no disk: QEMU is started with memory and a serial console, nothing else. So
 * the port hands the filesystem an array and calls it a device, which is
 * enough to have a tree of directories on this machine and a program in /bin,
 * and it is the whole of the difference between "the VFS is there" and "the
 * VFS has somewhere to keep a file".
 *
 * The array is static and in .bss on purpose: the allocator of the port is a
 * first fit list that lives above the kernel image, and a volume taken from it
 * would be one more block that a bug in the allocator can hand to somebody
 * else. The port knows how much memory the machine has and this is a fixed
 * part of it, the way the stack of the boot is.
 *
 * The medium is a model of the flash, not of a disk: a sector that has never
 * been written reads as all ones and has to be erased before it is written
 * again. MicroFS uses the first fact to recognise a volume that was never
 * formatted, and the tests of the filesystem use the same model.
 */
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/log.h"
#include "mfs/mfs.h"

/*
 * One sector is one block of the filesystem: MicroFS refuses a device that
 * speaks in another unit, because the layout of a volume is written in blocks
 * of MFS_BLOCK_SIZE and a device that reads more or less would hand out the
 * wrong bytes.
 *
 * Half a megabyte is far more than the files of this machine need (the image
 * of a program is a few hundred bytes and the table of the inodes is four
 * kilobytes) and small enough that a memory map of the machine does not
 * notice it.
 */
#define RAM_SECTOR_SIZE MFS_BLOCK_SIZE
#define RAM_SECTORS 512u
#define RAM_BYTES ((k_size_t)RAM_SECTOR_SIZE * RAM_SECTORS)

static k_u8_t g_ram_volume[RAM_BYTES] __attribute__((aligned(16)));

/*
 * A request for sectors that walk past the end of the volume is a caller that
 * misunderstands the medium: it is refused, not shortened, so that a
 * filesystem can never believe it wrote the block it did not write.
 */
static bool ram_range_ok(k_u32_t sector, k_size_t count)
{
    return sector < RAM_SECTORS && count <= (k_size_t)RAM_SECTORS - sector;
}

static int ram_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !ram_range_ok(sector, count)) {
        return -1;
    }

    memcpy(buf, g_ram_volume + (k_size_t)sector * RAM_SECTOR_SIZE, count * RAM_SECTOR_SIZE);

    return 0;
}

static int ram_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !ram_range_ok(sector, count)) {
        return -1;
    }

    memcpy(g_ram_volume + (k_size_t)sector * RAM_SECTOR_SIZE, buf, count * RAM_SECTOR_SIZE);

    return 0;
}

/*
 * Erase leaves the bits set, as the flash does. The filesystem writes whole
 * blocks and asks for no erase today; the operation exists because the
 * medium it models has one, and because the boot blanking the volume is the
 * first thing this device is asked to do (see port_x86_64_blockdev_init).
 */
static int ram_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    K_UNUSED(ctx);

    if (!ram_range_ok(sector, count)) {
        return -1;
    }

    memset(g_ram_volume + (k_size_t)sector * RAM_SECTOR_SIZE, 0xFF, count * RAM_SECTOR_SIZE);

    return 0;
}

/* The memory is written through: there is nothing to bring to the medium. */
static const struct k_blockdev_ops g_ram_ops = {
    .read = ram_read,
    .write = ram_write,
    .erase = ram_erase,
    .sync = NULL,
};

static const struct k_blockdev g_ram_device = {
    .name = "ram0",
    .sector_size = RAM_SECTOR_SIZE,
    .sectors = RAM_SECTORS,
    .ops = &g_ram_ops,
    .ctx = NULL, /* the volume is a variable of this file */
};

void port_x86_64_blockdev_init(void)
{
    /*
     * The machine has just started, so the volume has never been written:
     * erasing it is what makes the array say so. An array of zeros - which is
     * what .bss is - looks to MicroFS like a volume that somebody destroyed,
     * and the filesystem keeps such a volume and refuses to mount it rather
     * than format over the data it thinks it sees.
     */
    (void)ram_erase(NULL, 0, RAM_SECTORS);

    /* A successful registration says nothing, and the line that a person
     * reads is the mount of the volume a few lines later. */
    if (k_blockdev_register(&g_ram_device) != 0) {
        kerror("x86-64: cannot register the RAM disk");
    }
}
