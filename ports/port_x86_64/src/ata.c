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
 * A command is one transaction with the interrupts off (ata_transfer): the
 * timer of the machine and its keyboard are live while a disk works, and a
 * transfer that is preempted belongs to two tasks at once - the channel has
 * one command register and no notion of an owner. Every wait has a budget of
 * time (the counter of the processor, which the interrupts do not stop), a
 * failure is repeated once and reported with the number of the sector, and a
 * drive that does not answer costs the boot a message and not a hang.
 *
 * The writes collect in the cache of the drive; the filesystem flushes it
 * when it closes a file, makes a directory or unmounts (the sync operation
 * below), so the data of a call that returned is on the medium.
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
 * after this much time. The budget is time and not a number of polls, because
 * a medium behind the cache of the host can be slow - a sector goes through
 * the page cache and the platter of the machine that runs the emulator - and
 * a machine under load must not mistake slow for dead.
 *
 * The counter of the processor is the clock the driver may read while the
 * interrupts are off: the ticks of the kernel move in an interrupt, and no
 * interrupt is taken from the programming of a command to the end of its
 * transfer.
 */
#define ATA_TIMEOUT_MS 5000u

/*
 * The same budget for a machine whose processor the port could not calibrate:
 * a number of polls, generous for a medium of any speed and still finite.
 */
#define ATA_FALLBACK_POLLS 50000000u

/*
 * A command that fails is repeated once. The first failure of a boot is
 * often a stale state of the channel - an error latched by the command of a
 * device that was not ready - and the second attempt starts from a channel
 * that has been read and found idle.
 */
#define ATA_ATTEMPTS 2u

/* --- the bus, one word of the protocol at a time --- */

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
 * The budget of a wait, in the ticks of the time stamp counter; zero when the
 * port could not measure the processor.
 */
static k_u64_t ata_timeout_ticks(void)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info->cpu_hz == 0) {
        return 0;
    }

    return info->cpu_hz / 1000u * ATA_TIMEOUT_MS;
}

/*
 * Wait until the drive is not busy. With `mask` not zero the wait also asks
 * for one bit of the status - DRQ before a transfer. A drive that reports an
 * error, or one that does not answer in the budget, ends the wait.
 *
 * The wait is called with the interrupts off, so the polls are uninterrupted:
 * no task can take the channel while a command of this one is in flight, and
 * a slow medium is the only thing the loop waits for.
 */
static bool ata_wait(k_u8_t mask)
{
    k_u64_t budget = ata_timeout_ticks();
    k_u64_t start = port_rdtsc();
    k_u32_t polls = 0;

    for (;;) {
        k_u8_t status = port_inb(ATA_IO + ATA_STATUS);

        if ((status & ATA_STATUS_BSY) == 0) {
            if ((status & ATA_STATUS_ERR) != 0) {
                return false;
            }

            if (mask == 0 || (status & mask) != 0) {
                return true;
            }
        }

        if (budget != 0) {
            if (port_rdtsc() - start > budget) {
                return false;
            }
        } else if (++polls >= ATA_FALLBACK_POLLS) {
            return false;
        }

        __asm__ volatile("pause");
    }
}

/*
 * The words of one sector, as the bytes of the caller. The port is 16 bits
 * and the memory of the machine is little endian, so the words of the drive
 * are the words of the buffer - the run of `insl`/`outsl` needs no swap and
 * no byte at a time loop. The transfer is 32 bits wide (the 32-bit PIO of
 * the PC after 1994, and of the machine of this port): the two words of the
 * data register travel in one turn of the bus, and the low word of the
 * memory goes first, which is the order the port takes them.
 *
 * The drive asks for a transfer sector by sector (DRQ is raised for all of
 * them, the count of the sector is the unit of the FIFO), so the run is one
 * sector long and the caller waits between the runs.
 */
static void ata_read_words(k_u8_t *buf)
{
    port_insl(ATA_IO + ATA_DATA, buf, ATA_MEDIUM_SECTOR / 4u);
}

static void ata_write_words(const k_u8_t *buf)
{
    port_outsl(ATA_IO + ATA_DATA, buf, ATA_MEDIUM_SECTOR / 4u);
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

        /* The command is over when the drive says so, not when the last word
         * is read: the next command would be programmed into a channel that
         * is still finishing this one. */
        if (!ata_wait(0)) {
            return -1;
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

        /* A write is accepted when the drive leaves the transfer, and a write
         * fault is only visible here: the status of every sector of the chunk
         * is checked before the next chunk is programmed. */
        if (!ata_wait(0)) {
            return -1;
        }

        lba += (k_u32_t)chunk;
        sectors -= chunk;
    }

    return 0;
}

/*
 * One transaction of the channel: the interrupts off, the command, the
 * transfer and the drive idle again - with one more attempt when the first
 * one failed.
 *
 * The interrupts are off for the whole transaction on purpose. The timer of
 * this port fires every millisecond and the keyboard line is open (ADR 0008,
 * the VGA and PS/2 of the machine), so a transfer that is preempted is a
 * transfer that another task could interleave with its own: the drive has
 * one command register, and the state of the channel belongs to whoever
 * programmed it last. The price is a tick of the clock that is not counted
 * during a command; the disk does not keep time.
 *
 * A failed command leaves the error bit of the status latched and may leave
 * the channel busy. The registers are read (which acknowledges the line) and
 * the channel is waited out; the repeat starts only from an idle drive,
 * because a command written into a channel that is still transferring a
 * sector is a second failure and not a retry.
 */
static int ata_transfer(bool write, k_u32_t lba, const k_u8_t *buf, k_size_t sectors)
{
    k_u64_t flags = port_save_flags_and_cli();
    int rc = -1;
    k_u32_t attempt;

    for (attempt = 0; attempt < ATA_ATTEMPTS; attempt++) {
        rc = write ? ata_write_raw(lba, buf, sectors) : ata_read_raw(lba, (k_u8_t *)buf, sectors);

        if (rc == 0) {
            break;
        }

        (void)port_inb(ATA_IO + ATA_STATUS);
        (void)port_inb(ATA_IO + ATA_ERROR);

        if (!ata_wait(0)) {
            break;
        }
    }

    port_restore_flags(flags);

    if (rc != 0) {
        kerror("ata0: the %s of %u sector(s) at %u failed (status 0x%02x, error 0x%02x)",
               write ? "write" : "read", (unsigned)sectors, (unsigned)lba,
               (unsigned)port_inb(ATA_IO + ATA_STATUS), (unsigned)port_inb(ATA_IO + ATA_ERROR));
    }

    return rc;
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

    return ata_transfer(false, sector * ATA_SECTORS_PER_BLOCK, (const k_u8_t *)buf,
                        count * ATA_SECTORS_PER_BLOCK);
}

static int ata_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !ata_range_ok(sector, count)) {
        return -1;
    }

    return ata_transfer(true, sector * ATA_SECTORS_PER_BLOCK, (const k_u8_t *)buf,
                        count * ATA_SECTORS_PER_BLOCK);
}

/*
 * The drive has a cache of its own: the writes of a file collect in it, and
 * the cache is emptied by the flush command. The filesystem calls this when
 * it closes a file, makes a directory or unmounts the volume, so the data of
 * a command that returned is on the medium even if the machine stops.
 *
 * The flush is a command like any other and is waited out the same way: the
 * drive is busy while it empties the cache, and the busy bit - not only the
 * request of a transfer - says when it is done.
 */
static int ata_sync(void *ctx)
{
    k_u64_t flags;
    int rc = -1;
    k_u32_t attempt;

    K_UNUSED(ctx);

    flags = port_save_flags_and_cli();

    for (attempt = 0; attempt < ATA_ATTEMPTS; attempt++) {
        if (ata_command(ATA_CMD_FLUSH, 0, 0) && ata_wait(0)) {
            rc = 0;
            break;
        }

        (void)port_inb(ATA_IO + ATA_STATUS);
        (void)port_inb(ATA_IO + ATA_ERROR);
    }

    port_restore_flags(flags);

    if (rc != 0) {
        kerror("ata0: the flush of the drive failed (status 0x%02x)",
               (unsigned)port_inb(ATA_IO + ATA_STATUS));
    }

    return rc;
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

    /* The identity is 256 words of the port, and the buffer holds them in
     * the order the drive sends them: one run, as the sector of a disk. */
    port_insw(ATA_IO + ATA_DATA, identity, K_ARRAY_SIZE(identity));

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
    const struct k_blockdev *disk;
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

    /*
     * The platter is the slowest thing the machine has, and a filesystem
     * touches the metadata of a volume again for every block it allocates:
     * the block layer keeps the last sectors of the disk between the
     * filesystem and the port, and holds the writes back until a sync or
     * until the cache needs the room - a run of sectors then leaves it as
     * one command of the drive. The filesystems of this machine sync when
     * they close a file and when they unmount, so what a call returned
     * survives the machine.
     */
    if (k_blockdev_register_cached(&g_ata_device, true) != 0) {
        kerror("ata0: cannot register the disk");
        return;
    }

    kinfo("ata0: %u MB on the primary channel", (unsigned)(g_medium_sectors / 2048u));

    /*
     * The scan reads through the device the table hands out, not through the
     * structure of the platform: that is the view with the cache, and the
     * partitions keep the parent they were found on - so a read of a
     * partition reaches the cache of the disk below it.
     */
    disk = k_blockdev_find(g_ata_device.name);

    if (disk == NULL) {
        disk = &g_ata_device;
    }

    partitions = partition_scan(disk);

    if (partitions == KERR_NOENT) {
        kinfo("ata0: no partition table, the disk is not used");
        return;
    }

    if (partitions < 0) {
        kerror("ata0: the partition table is not usable (%d)", partitions);
        return;
    }

    /* The first partition is the root of the machine. */
    first = partition_name(disk, 0);

    if (first == NULL) {
        return;
    }

    g_root = first;
    kinfo("x86-64: the root volume is %s (%d partition(s))", first, partitions);
}

/*
 * Where the filesystem of the boot lives: the root image the boot loader
 * handed the kernel when it came from an ISO, the first partition of the disk
 * when there is one, the volume in the memory when there is neither.
 */
const char *port_x86_64_root_device(void)
{
    const char *boot = port_x86_64_module_root();

    if (boot != NULL) {
        return boot;
    }

    return g_root != NULL ? g_root : "ram0";
}
