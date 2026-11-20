/*
 * Micro OS — the x86-64 port: a disk on the ATA channel, read and written by
 * the processor itself.
 *
 * The machine of this port had no disk until ADR 0008: the filesystem lived
 * in an array of the memory (blockdev_ram.c) and every boot made it again.
 * This file is the first medium that survives a reset - a raw image that QEMU
 * attaches to the primary IDE channel, the interface every PC had from 1984
 * to the end of the parallel ATA.
 *
 * The driver speaks PIO and not DMA. A DMA transfer needs the physical
 * address of a buffer, and this kernel has no contract for one yet: the port
 * runs on the identity map of the loader, and the translation of an address
 * is one of the things the paging work is about to change. PIO hands the
 * bytes to the processor through the port 0x1F0, where a virtual address is
 * the address, and polling the status register instead of waiting for the
 * interrupt of the channel keeps the driver out of the interrupt controller.
 * It is slower and it is the honest first step: a disk that works on one
 * address map is worth more than a fast one that faults on the next.
 *
 * The drive is programmed in LBA28, which addresses 128 GiB, and it is split
 * into chunks of 32 sectors so that one command never fills the FIFO of the
 * controller of a real machine.
 *
 * A sector of the disk is 512 bytes and a block of MicroFS is 1 KiB: the
 * device is registered in the units of the filesystem (see the beginning of
 * blockdev_ram.c), and the two sectors of a block are read by one command.
 */
#include "port_x86_64.h"

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/partition.h"

#include "mfs/mfs.h"

/* --- the registers of the primary channel --- */

#define ATA_IO 0x1F0u
#define ATA_CONTROL 0x3F6u /* the alternate status, and the device control */
/* The drive register: bit 6 says that the address is LBA, bit 4 chooses the
 * master, and the low four bits are the top bits of the address. */
#define ATA_DRIVE_LBA 0xE0u

/* The identify command is addressed without LBA mode: the drive answers the
 * same way, but the register is the one the 1986 standard describes. */
#define ATA_DRIVE_MASTER 0xA0u

#define ATA_DATA 0u
#define ATA_ERROR 1u
#define ATA_SECTOR_COUNT 2u
#define ATA_LBA_LOW 3u
#define ATA_LBA_MID 4u
#define ATA_LBA_HIGH 5u
#define ATA_DRIVE 6u
#define ATA_STATUS 7u
#define ATA_COMMAND 7u

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_BSY 0x80u

#define ATA_CMD_READ 0x20u
#define ATA_CMD_WRITE 0x30u
#define ATA_CMD_FLUSH 0xE7u
#define ATA_CMD_IDENTIFY 0xECu

/* The number of the bits of LBA the drive is programmed with. */
#define ATA_LBA28_SECTORS 0x10000000u

/* One command moves at most this many sectors; the rest go in another. */
#define ATA_CHUNK 32u

/* The sectors of the medium that one block of the filesystem covers. */
#define ATA_MEDIUM_SECTOR 512u
#define ATA_SECTORS_PER_BLOCK (MFS_BLOCK_SIZE / ATA_MEDIUM_SECTOR)

_Static_assert(MFS_BLOCK_SIZE % ATA_MEDIUM_SECTOR == 0,
               "a block of the filesystem is not a whole number of disk sectors");

/*
 * A device that does not answer must not hang the boot: every wait gives up
 * after this many reads of the status. A real transfer of one sector is a
 * few dozen polls, so the number is generous and still finite.
 */
#define ATA_POLL_LIMIT 2000000u

/* --- the bus, one word at a time --- */

static inline void ata_outw(k_u16_t port, k_u16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline k_u16_t ata_inw(k_u16_t port)
{
    k_u16_t value;

    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/*
 * After the drive is selected the registers of the channel need 400 ns to
 * settle; four reads of the alternate status spend it without a timer.
 */
static void ata_delay(void)
{
    (void)port_inb(ATA_CONTROL);
    (void)port_inb(ATA_CONTROL);
    (void)port_inb(ATA_CONTROL);
    (void)port_inb(ATA_CONTROL);
}

/*
 * Wait until the drive is not busy. With `mask` not zero the wait also asks
 * for one bit of the status - DRQ before a transfer. A drive that reports an
 * error, or one that never answers, ends the wait.
 */
static bool ata_wait(k_u8_t mask)
{
    k_u32_t i;

    for (i = 0; i < ATA_POLL_LIMIT; i++) {
        k_u8_t status = port_inb(ATA_IO + ATA_STATUS);

        if ((status & ATA_STATUS_BSY) != 0) {
            continue;
        }

        if ((status & ATA_STATUS_ERR) != 0) {
            return false;
        }

        if (mask == 0 || (status & mask) != 0) {
            return true;
        }
    }

    return false;
}

/* The words of one sector, as the bytes of the caller: the port is 16 bits
 * and the memory of the machine is little endian, so low byte first. */
static void ata_read_words(k_u8_t *buf)
{
    k_size_t i;

    for (i = 0; i < ATA_MEDIUM_SECTOR; i += 2u) {
        k_u16_t word = ata_inw(ATA_IO + ATA_DATA);

        buf[i] = (k_u8_t)(word & 0xFFu);
        buf[i + 1u] = (k_u8_t)(word >> 8);
    }
}

static void ata_write_words(const k_u8_t *buf)
{
    k_size_t i;

    for (i = 0; i < ATA_MEDIUM_SECTOR; i += 2u) {
        k_u16_t word = (k_u16_t)((k_u16_t)buf[i] | ((k_u16_t)buf[i + 1u] << 8));

        ata_outw(ATA_IO + ATA_DATA, word);
    }
}

/*
 * Program a command for `count` sectors at `lba`: the drive, the address in
 * three registers and the four bits of the high nibble, then the command.
 */
static bool ata_command(k_u8_t command, k_u32_t lba, k_u32_t count)
{
    port_outb(ATA_IO + ATA_DRIVE, (k_u8_t)(ATA_DRIVE_LBA | ((lba >> 24) & 0x0Fu)));
    ata_delay();

    if (!ata_wait(0)) {
        return false;
    }

    port_outb(ATA_IO + ATA_SECTOR_COUNT, (k_u8_t)(count & 0xFFu));
    port_outb(ATA_IO + ATA_LBA_LOW, (k_u8_t)(lba & 0xFFu));
    port_outb(ATA_IO + ATA_LBA_MID, (k_u8_t)((lba >> 8) & 0xFFu));
    port_outb(ATA_IO + ATA_LBA_HIGH, (k_u8_t)((lba >> 16) & 0xFFu));
    port_outb(ATA_IO + ATA_COMMAND, command);

    return true;
}

static int ata_read_raw(k_u32_t lba, k_u8_t *buf, k_size_t sectors)
{
    while (sectors > 0) {
        k_size_t chunk = sectors > ATA_CHUNK ? ATA_CHUNK : sectors;
        k_size_t i;

        if (!ata_command(ATA_CMD_READ, lba, (k_u32_t)chunk)) {
            return -1;
        }

        for (i = 0; i < chunk; i++) {
            if (!ata_wait(ATA_STATUS_DRQ)) {
                return -1;
            }

            ata_read_words(buf);
            buf += ATA_MEDIUM_SECTOR;
        }

        lba += (k_u32_t)chunk;
        sectors -= chunk;
    }

    return 0;
}

static int ata_write_raw(k_u32_t lba, const k_u8_t *buf, k_size_t sectors)
{
    while (sectors > 0) {
        k_size_t chunk = sectors > ATA_CHUNK ? ATA_CHUNK : sectors;
        k_size_t i;

        if (!ata_command(ATA_CMD_WRITE, lba, (k_u32_t)chunk)) {
            return -1;
        }

        for (i = 0; i < chunk; i++) {
            if (!ata_wait(ATA_STATUS_DRQ)) {
                return -1;
            }

            ata_write_words(buf);
            buf += ATA_MEDIUM_SECTOR;
        }

        lba += (k_u32_t)chunk;
        sectors -= chunk;
    }

    return 0;
}

/* --- the device itself --- */

static k_u32_t g_medium_sectors; /* the sectors of 512 bytes of the drive */
static k_u32_t g_sectors;        /* the blocks of the filesystem on it */
static const char *g_root;       /* the partition the boot mounts, if any */

static bool ata_range_ok(k_u32_t sector, k_size_t count)
{
    return g_sectors != 0 && sector < g_sectors && count <= (k_size_t)g_sectors - sector;
}

static int ata_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !ata_range_ok(sector, count)) {
        return -1;
    }

    return ata_read_raw(sector * ATA_SECTORS_PER_BLOCK, (k_u8_t *)buf,
                        count * ATA_SECTORS_PER_BLOCK);
}

static int ata_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !ata_range_ok(sector, count)) {
        return -1;
    }

    return ata_write_raw(sector * ATA_SECTORS_PER_BLOCK, (const k_u8_t *)buf,
                         count * ATA_SECTORS_PER_BLOCK);
}

/*
 * The drive has a cache of its own: a write is finished when the cache is
 * empty, and the cache is emptied by the command of the drive. Every write
 * ends with one, which is slower than a burst of writes and it is the price
 * of a file that is on the platter when the command returns.
 */
static int ata_sync(void *ctx)
{
    K_UNUSED(ctx);

    if (!ata_command(ATA_CMD_FLUSH, 0, 0)) {
        return -1;
    }

    return ata_wait(0) ? 0 : -1;
}

static const struct k_blockdev_ops g_ata_ops = {
    .read = ata_read,
    .write = ata_write,
    .erase = NULL, /* a disk keeps its sectors until they are written */
    .sync = ata_sync,
};

static struct k_blockdev g_ata_device = {
    .name = "ata0",
    .sector_size = MFS_BLOCK_SIZE,
    .sectors = 0, /* the size of the drive, read by the identify below */
    .ops = &g_ata_ops,
    .ctx = NULL,
};

/*
 * Ask the drive who it is. The identify command answers with 256 words, and
 * the two of them after the first sixty are the number of the sectors of 512
 * bytes the drive has. A channel with nothing on it answers zero, a packet
 * device (a CD-ROM) puts its signature in two registers - neither is a disk
 * this driver can use.
 */
static bool ata_identify(k_u32_t *medium_sectors)
{
    k_u16_t identity[256];
    k_u32_t capacity;
    k_u8_t status;
    k_size_t i;

    port_outb(ATA_IO + ATA_DRIVE, ATA_DRIVE_MASTER);
    ata_delay();
    port_outb(ATA_IO + ATA_SECTOR_COUNT, 0);
    port_outb(ATA_IO + ATA_LBA_LOW, 0);
    port_outb(ATA_IO + ATA_LBA_MID, 0);
    port_outb(ATA_IO + ATA_LBA_HIGH, 0);
    port_outb(ATA_IO + ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();

    status = port_inb(ATA_IO + ATA_STATUS);
    if (status == 0) {
        return false;
    }

    if (!ata_wait(0)) {
        return false;
    }

    if (port_inb(ATA_IO + ATA_LBA_MID) != 0 || port_inb(ATA_IO + ATA_LBA_HIGH) != 0) {
        return false;
    }

    if (!ata_wait(ATA_STATUS_DRQ)) {
        return false;
    }

    for (i = 0; i < K_ARRAY_SIZE(identity); i++) {
        identity[i] = ata_inw(ATA_IO + ATA_DATA);
    }

    capacity = (k_u32_t)identity[60] | ((k_u32_t)identity[61] << 16);

    if (capacity == 0) {
        return false;
    }

    if (capacity > ATA_LBA28_SECTORS) {
        capacity = ATA_LBA28_SECTORS;
    }

    *medium_sectors = capacity;

    return true;
}

/*
 * The disk of the port, if QEMU was given one. The root volume of the boot is
 * the first partition of it (ADR 0008): the whole disk is not a volume, and
 * the boot says so when the table is missing instead of formatting over it.
 */
void port_x86_64_disk_init(void)
{
    const char *first;
    int partitions;

    if (!ata_identify(&g_medium_sectors)) {
        /* No drive on the channel is the normal state of make x86-run: the
         * machine keeps its volume in the memory (blockdev_ram.c). */
        return;
    }

    if (g_medium_sectors < ATA_SECTORS_PER_BLOCK ||
        (g_medium_sectors % ATA_SECTORS_PER_BLOCK) != 0) {
        kerror("ata0: the drive is not a whole number of %u-byte blocks", (unsigned)MFS_BLOCK_SIZE);
        return;
    }

    g_sectors = g_medium_sectors / ATA_SECTORS_PER_BLOCK;
    g_ata_device.sectors = g_sectors;

    if (k_blockdev_register(&g_ata_device) != 0) {
        kerror("ata0: cannot register the disk");
        return;
    }

    kinfo("ata0: %u MB on the primary channel", (unsigned)(g_medium_sectors / 2048u));

    partitions = partition_scan(&g_ata_device);

    if (partitions == KERR_NOENT) {
        kinfo("ata0: no partition table, the disk is not used");
        return;
    }

    if (partitions < 0) {
        kerror("ata0: the partition table is not usable (%d)", partitions);
        return;
    }

    /* The first partition is the root of the machine. */
    first = partition_name(&g_ata_device, 0);

    if (first == NULL) {
        return;
    }

    g_root = first;
    kinfo("x86-64: the root volume is %s (%d partition(s))", first, partitions);
}

/*
 * Where the filesystem of the boot lives: the first partition of the disk
 * when there is one, the volume in the memory when there is not.
 */
const char *port_x86_64_root_device(void)
{
    return g_root != NULL ? g_root : "ram0";
}
