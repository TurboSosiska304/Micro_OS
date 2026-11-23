/*
 * Micro OS — the x86-64 port: a disk on the ATA channel, read and written by
 * the processor or by the bus master of the channel.
 *
 * The machine of this port had no disk until ADR 0008: the filesystem lived
 * in an array of the memory (blockdev_ram.c) and every boot made it again.
 * This file is the first medium that survives a reset - a raw image that QEMU
 * attaches to the primary IDE channel, the interface every PC had from 1984
 * to the end of the parallel ATA.
 *
 * The driver speaks both ways the channel knows. PIO hands the bytes to the
 * processor through the port 0x1F0 and is the path of every machine: it needs
 * nothing but the port, and a virtual address is the address. Bus-master DMA
 * (ADR 0032) hands the controller a table of physical addresses - the PRDT -
 * and the controller moves the bytes while the processor does something else;
 * the transfer ends with the interrupt of the channel, IRQ 14, and not with a
 * poll of the status register. The DMA path is used when the machine has the
 * function (a PCI IDE function with the bus-master bit and a BAR of
 * registers), when the drive says it can (the identify data), and when every
 * page of the buffer has a physical address the controller can be given. When
 * one of the three says no, the command is a PIO command: slower, and the one
 * that cannot be wrong about an address.
 *
 * A command is one transaction of the channel, and the channel has one
 * command register and no notion of an owner. PIO keeps the transaction with
 * the interrupts off: the timer of the machine and its keyboard are live
 * while a disk works, and a transfer that is preempted belongs to two tasks at
 * once. DMA cannot do that - the transfer is a wait of an interrupt - so the
 * transaction is serialised by a mutex of the driver instead, and the
 * interrupts stay on. Every wait has a budget of time (the counter of the
 * processor, which the interrupts do not stop), a failure is repeated once by
 * the PIO path and reported with the number of the sector, and a drive that
 * does not answer costs the boot a message and not a hang.
 *
 * The writes collect in the cache of the drive; the filesystem flushes it
 * when it closes a file, makes a directory or unmounts (the sync operation
 * below), so the data of a call that returned is on the medium. The block
 * layer above this driver holds the writes of a run back in a cache of its
 * own (kernel/blockdev.c), and what reaches this file is already a run.
 *
 * The drive is programmed in LBA28, which addresses 128 GiB, and a command
 * moves at most 32 sectors of PIO or 128 sectors of DMA; the rest go in
 * another command.
 *
 * A sector of the disk is 512 bytes and a block of MicroFS is 1 KiB: the
 * device is registered in the units of the filesystem (see the beginning of
 * blockdev_ram.c), and the two sectors of a block are read by one command.
 */
#include "port_x86_64.h"

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/partition.h"

#include "mfs/mfs.h"

#include "port_x86_64_pci.h"

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
#define ATA_CMD_READ_DMA 0xC8u  /* LBA28, multiword DMA */
#define ATA_CMD_WRITE_DMA 0xCAu /* the same, the other direction */
#define ATA_CMD_FLUSH 0xE7u
#define ATA_CMD_IDENTIFY 0xECu

/* The number of the bits of LBA the drive is programmed with. */
#define ATA_LBA28_SECTORS 0x10000000u

/* One PIO command moves at most this many sectors; the rest go in another. */
#define ATA_CHUNK 32u

/* --- the bus master of the primary channel --- */

/*
 * The five registers of the function, in the window its BAR names: a command
 * byte, a status word, and the physical address of the table of the
 * descriptors. The direction of the transfer is a bit of the command: the
 * controller reads the memory when the bit is set and writes it when it is
 * clear, which is the opposite of the way the words "read" and "write" are
 * used for the memory.
 */
#define ATA_BM_COMMAND 0x00u
#define ATA_BM_STATUS 0x02u
#define ATA_BM_PRDT 0x04u

#define ATA_BM_CMD_START 0x01u
#define ATA_BM_CMD_READ 0x08u /* the disk to the memory */

#define ATA_BM_STATUS_ACTIVE 0x01u
#define ATA_BM_STATUS_ERROR 0x02u
#define ATA_BM_STATUS_IRQ 0x04u
#define ATA_BM_STATUS_MASK (ATA_BM_STATUS_ERROR | ATA_BM_STATUS_IRQ)

/* The two bits of the status a drive that can move the bus sets: one per
 * drive of the channel. */
#define ATA_BM_STATUS_DRIVE0 0x20u

/* The class and the interface of a PCI IDE function that can master the
 * bus: 01 storage, 01 IDE, and bit 7 of the programming interface. */
#define PCI_CLASS_STORAGE 0x01u
#define PCI_SUBCLASS_IDE 0x01u
#define PCI_PROG_IF_BUS_MASTER 0x80u

/*
 * One entry of the PRDT: the physical address of a piece of the buffer and
 * its length in bytes (a zero means 65536), then the two flags of the last
 * entry and of the reset. A piece never crosses a page (ADR 0032 §2), so a
 * command of 64 KiB is at most seventeen entries and every one of them is
 * inside the page its address names.
 */
struct ata_prd {
    k_u32_t address;
    k_u16_t count;
    k_u16_t flags;
};

#define ATA_PRD_END 0x8000u
#define ATA_PRD_MAX 17u
#define ATA_PAGE_SIZE 4096u

/* One DMA command moves at most this many sectors: 64 KiB, the ceiling of a
 * table of seventeen pages. */
#define ATA_DMA_CHUNK 128u

/* --- the interrupt of the channel ---------------------------------------- */

/* The line of the primary channel is the fourteenth of the 8259, which is the
 * sixth of the slave: the vector is the one the master's table starts at,
 * plus fourteen, and the slave reaches the processor through the master. */
#define IRQ_BASE 0x20u
#define IRQ_ATA 14u

#define PIC1_COMMAND 0x20u
#define PIC2_COMMAND 0xA0u
#define PIC_EOI 0x20u

/* The handler of the line, in interrupts.S, and the C body of it, below. */
extern void isr_irq_ata(void);
void port_x86_64_irq_ata(k_u64_t *frame);

/*
 * The cache of the block layer above this disk: 512 sectors of 1 KiB, half a
 * megabyte. A run of the medium is what a bus master moves in one command,
 * and the working set of a volume - the bitmap of a group, the table of the
 * inodes, the descriptors - fits inside this with room for the file.
 */
#define ATA_CACHE_SECTORS 512u

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

/* --- the bus master ------------------------------------------------------- */

/*
 * The state of the function, decided once at the boot: the window of its
 * registers (zero when the machine has no bus master the driver can use), the
 * table of the descriptors, and the number of the interrupts of the channel
 * that the handler of IRQ 14 has seen.
 */
static k_u16_t g_dma_io;
static struct ata_prd g_prdt[ATA_PRD_MAX] __attribute__((aligned(ATA_PAGE_SIZE)));
static volatile k_u32_t g_dma_irqs;
static bool g_dma_live;              /* the line of the channel is open and has a handler */
static volatile k_u8_t g_dma_status; /* the status the handler read, for a wait */
static bool g_dma_warned;            /* the fallback to PIO is in the log */
static k_mutex_t g_ata_lock = K_MUTEX_INITIALIZER;

/*
 * The registers of the function, one access at a time. The command and the
 * status are bytes and the address of the table is a word of 32 bits, and the
 * controller of the PC insists on both: a wider read of a byte register is
 * not a number with the byte in it but a refused access, and a narrower write
 * of the address is a piece of it. The four calls keep that knowledge in one
 * place.
 */
static void ata_bm_command(k_u8_t value)
{
    port_outb(g_dma_io + ATA_BM_COMMAND, value);
}

static k_u8_t ata_bm_status(void)
{
    return port_inb(g_dma_io + ATA_BM_STATUS);
}

static void ata_bm_status_clear(k_u8_t bits)
{
    port_outb(g_dma_io + ATA_BM_STATUS, bits);
}

static void ata_bm_table(k_u32_t address)
{
    port_outl(g_dma_io + ATA_BM_PRDT, address);
}

/*
 * The physical address behind a byte the transfer names (ADR 0032 §1). The
 * window of the programs is the only memory of this machine whose virtual
 * address is not its physical one, and the page tables of the moment are the
 * only place that knows where its frame is. Everything else is arithmetic:
 * the heap is the identity map and the image is a fixed offset.
 *
 * A page that is not there answers zero, which no caller may use: the
 * transfer falls back to PIO instead of handing the controller a number that
 * names something else.
 */
static k_u64_t ata_phys(k_u64_t address)
{
    if (address >= PORT_X86_64_USER_BASE && address < PORT_X86_64_USER_LIMIT) {
        void *frame = port_x86_64_paging_phys(port_x86_64_current_cr3(),
                                              address & ~(k_u64_t)(ATA_PAGE_SIZE - 1u));

        if (frame == NULL) {
            return 0;
        }

        return (k_u64_t)(uintptr_t)frame | (address & (ATA_PAGE_SIZE - 1u));
    }

    return port_x86_64_bus_phys((const void *)(uintptr_t)address);
}

/*
 * Fill the table with the pieces of `sectors` sectors at `buf`, one entry per
 * page, and answer the number of sectors it describes. Zero is "this buffer
 * is not one a controller may be given" - a page that does not translate, a
 * frame above the four gigabytes the register can name, a table too small for
 * the request - and the caller then speaks PIO.
 *
 * The pieces are cut at the page boundary because two virtual pages are not
 * two adjacent frames (ADR 0032 §2). A buffer at an odd address is refused
 * for the reason the standard gives: the count of a descriptor is a count of
 * words.
 */
static k_size_t ata_prdt_fill(const k_u8_t *buf, k_size_t sectors)
{
    k_size_t total = sectors * ATA_MEDIUM_SECTOR;
    k_size_t done = 0;
    k_size_t entries = 0;

    /*
     * A buffer is not aligned to anything in particular: the filesystem
     * hands the driver a member of a structure, and the sector the command
     * starts at is a number and not a promise about the address. So the
     * pieces are counted in bytes and a page boundary may fall in the middle
     * of a sector - the controller moves a byte count and does not care -
     * and the table is only finished when the whole request is described.
     */
    while (done < total && entries < ATA_PRD_MAX) {
        k_u64_t address = (k_u64_t)(uintptr_t)(buf + done);
        k_size_t left = total - done;
        k_size_t inside = (k_size_t)(address & (ATA_PAGE_SIZE - 1u));
        k_size_t piece = ATA_PAGE_SIZE - inside;
        k_u64_t physical;

        /* The count of a descriptor is a count of words: a buffer at an odd
         * address is a buffer the controller is not given. */
        if ((address & 1u) != 0) {
            return 0;
        }

        if (piece > left) {
            piece = left;
        }

        physical = ata_phys(address);

        if (physical == 0 || physical > 0xFFFFFFFFu) {
            return 0;
        }

        g_prdt[entries].address = (k_u32_t)physical;
        g_prdt[entries].count = (k_u16_t)piece;
        g_prdt[entries].flags = 0;

        entries++;
        done += piece;
    }

    /* Seventeen pieces cover 64 KiB from any alignment, so a request of one
     * command always fits. A table that ran out describes a request that was
     * not cut, and a half-described command is a transfer of the wrong bytes:
     * the caller speaks PIO instead. */
    if (done < total) {
        return 0;
    }

    g_prdt[entries - 1u].flags = ATA_PRD_END;

    return done / ATA_MEDIUM_SECTOR;
}

/*
 * Wait for the end of a DMA command: the interrupt of the channel, and the
 * status of the engine as the truth of it. The core is halted with the
 * interrupts on, so the line of the disk wakes it at once and the tick of the
 * timer - a millisecond later at the most - is what keeps a machine that
 * never raises the line from sleeping for ever. The registers are read on
 * every wake and not in a spin: a transfer of 64 KiB is one or two of them.
 */
static bool ata_dma_wait(void)
{
    k_u64_t budget = ata_timeout_ticks();
    k_u64_t start = port_rdtsc();
    k_u32_t polls = 0;

    for (;;) {
        k_u64_t flags = port_save_flags_and_cli();
        k_u8_t status = ata_bm_status();

        if ((status & ATA_BM_STATUS_ERROR) != 0) {
            port_restore_flags(flags);
            return false;
        }

        /* The interrupt of the engine and the end of its work: a transfer
         * that has raised the line but is still writing the last descriptor
         * is not over. */
        if ((status & ATA_BM_STATUS_IRQ) != 0 && (status & ATA_BM_STATUS_ACTIVE) == 0) {
            port_restore_flags(flags);
            return true;
        }

        if (budget != 0) {
            if (port_rdtsc() - start > budget) {
                port_restore_flags(flags);
                return false;
            }
        } else if (++polls >= ATA_FALLBACK_POLLS) {
            port_restore_flags(flags);
            return false;
        }

        /*
         * Sleep until the next interrupt with the interrupts off around the
         * check and on around the sleep, and the two instructions are one:
         * the processor does not take an interrupt between `sti` and the
         * instruction after it, so the line of the disk cannot be raised
         * between the status above and the `hlt` below and then wait for the
         * tick of the timer - a millisecond of sleep for every command is
         * seven megabytes a second and not a hundred.
         */
        __asm__ volatile("sti; hlt" : : : "memory");
        port_restore_flags(flags);
    }
}

/* Stop the engine and take the line of the function down. The bits of the
 * status are written back as ones: the register clears a bit that is set. */
static k_u8_t ata_dma_stop(void)
{
    k_u8_t status;

    ata_bm_command(0);
    status = ata_bm_status();
    ata_bm_status_clear((k_u8_t)(status & ATA_BM_STATUS_MASK));

    return status;
}

/*
 * One command of the bus master: the table, the direction, the start and the
 * command of the drive. `sectors` is what the table describes and never more
 * than the engine may name at once.
 *
 * The table is programmed before the start bit, and the start bit before the
 * command: the drive enters the data phase of a command that already has an
 * engine behind it, which is the order the standard describes and the one a
 * controller that checks the two against each other expects.
 */
static bool ata_dma_start(bool write, k_u32_t lba, k_size_t sectors)
{
    k_u8_t direction = write ? 0u : ATA_BM_CMD_READ;

    ata_bm_command(0);
    ata_bm_status_clear(ATA_BM_STATUS_MASK);
    ata_bm_table((k_u32_t)port_x86_64_kernel_phys(g_prdt));
    ata_bm_command(direction);
    ata_bm_command((k_u8_t)(direction | ATA_BM_CMD_START));

    if (!ata_command(write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA, lba, (k_u32_t)sectors)) {
        (void)ata_dma_stop();
        return false;
    }

    return true;
}

/*
 * The transfer of the bus master, a table at a time. Answers 0 for a command
 * that reached the medium, -1 for one that failed and -2 for a buffer the
 * controller may not be given; the caller repeats the whole thing in PIO for
 * either failure, because a command that left the drive in a state nobody
 * understands is exactly what the other path is for.
 */
static int ata_dma_transfer(bool write, k_u32_t lba, const k_u8_t *buf, k_size_t sectors)
{
    while (sectors > 0) {
        k_size_t chunk = sectors > ATA_DMA_CHUNK ? ATA_DMA_CHUNK : sectors;
        k_size_t described = ata_prdt_fill(buf, chunk);
        k_u8_t status;

        if (described == 0) {
            return -2;
        }

        if (!ata_dma_start(write, lba, described)) {
            return -1;
        }

        if (!ata_dma_wait()) {
            (void)ata_dma_stop();
            return -1;
        }

        status = ata_dma_stop();
        g_dma_status = port_inb(ATA_IO + ATA_STATUS);

        if ((status & ATA_BM_STATUS_ERROR) != 0 || (g_dma_status & ATA_STATUS_ERR) != 0) {
            return -1;
        }

        lba += (k_u32_t)described;
        buf += described * ATA_MEDIUM_SECTOR;
        sectors -= described;
    }

    return 0;
}

/*
 * The interrupt of the primary channel, IRQ 14. The drive is acknowledged by
 * the read of its status - a line that stays up is a storm of interrupts -
 * and the bus master keeps its own bits until the transfer that started it
 * reads them, so the handler does not touch the function: the engine's
 * status is the answer of the wait, and clearing it here would lose it.
 *
 * The line of the disk is the sixth of the slave of the 8259, and the slave
 * reaches the processor through the second line of the master: both chips are
 * told that the interrupt is over.
 */
void port_x86_64_irq_ata(k_u64_t *frame)
{
    K_UNUSED(frame);

    g_dma_status = port_inb(ATA_IO + ATA_STATUS);
    g_dma_irqs++;

    port_outb(PIC2_COMMAND, PIC_EOI);
    port_outb(PIC1_COMMAND, PIC_EOI);
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
 * The PIO path of one transaction: the interrupts off, the command, the
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
static int ata_pio_transfer(bool write, k_u32_t lba, const k_u8_t *buf, k_size_t sectors)
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

    return rc;
}

/*
 * One transaction of the channel, by the bus master when the machine has one
 * and the buffer may be handed to it, and by the processor otherwise.
 *
 * The DMA path cannot keep the interrupts off - the end of the transfer is
 * the interrupt - so the channel is serialised by a mutex instead: the timer
 * may preempt the task that waits, and another task that wants the disk finds
 * the lock held and gives the processor away. A machine that falls back keeps
 * the section of PIO and is exactly the machine of 1.5.0.
 *
 * The DMA path is left alone for the rest of the session by a buffer it
 * cannot name: the filesystem hands the driver the buffer of a program
 * (ADR 0032 §1), and a program may hand it a pointer of the heap of the
 * kernel or one of its own pages at any time. The translation is asked per
 * transfer and the answer is not remembered.
 */
static int ata_transfer(bool write, k_u32_t lba, const k_u8_t *buf, k_size_t sectors)
{
    int rc;

    if (g_dma_live) {
        k_u64_t flags = port_save_flags_and_cli();
        bool live = (flags & 0x200u) != 0; /* the interrupts of the moment */

        port_restore_flags(flags);

        if (live && k_mutex_lock(&g_ata_lock) == 0) {
            rc = ata_dma_transfer(write, lba, buf, sectors);

            k_mutex_unlock(&g_ata_lock);

            if (rc == 0) {
                return 0;
            }

            /* The command of the controller failed or the buffer is not one
             * it may be given: the same command again, through the port. One
             * line, the first time: a filesystem that hands the driver the
             * buffer of a program does it on every call, and a sentence per
             * sector would bury the log. */
            if (!g_dma_warned) {
                g_dma_warned = true;
                kwarn("ata0: the %s of %u sector(s) at %u goes through PIO",
                      write ? "write" : "read", (unsigned)sectors, (unsigned)lba);
            }
        }
    }

    rc = ata_pio_transfer(write, lba, buf, sectors);

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
 *
 * The word 49 says what the drive can do: bit 8 is the multiword DMA the
 * bus-master commands of this driver ask for. A drive without it is a drive
 * of the PIO path, whatever the controller can do.
 */
static bool ata_identify(k_u32_t *medium_sectors, bool *dma_capable)
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
    *dma_capable = (identity[49] & 0x0100u) != 0;

    return true;
}

/*
 * The bus master of the primary channel, if the machine has one: the PCI
 * function of class 01/01 that says it can master the bus, the fifth BAR of
 * it (the five registers) and the bit of the command register that lets it.
 *
 * The window of the registers is an I/O window of sixteen bytes; a function
 * that names a memory window there is not the function of the legacy
 * channel, and a machine with no such function keeps PIO. The answer is the
 * port of the registers, zero for "no bus master".
 */
static k_u16_t ata_dma_probe(void)
{
    struct port_x86_64_pci_device ide;
    k_u32_t bar;
    k_u16_t port;

    if (!port_x86_64_pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, PCI_PROG_IF_BUS_MASTER,
                                    &ide)) {
        return 0;
    }

    bar = port_x86_64_pci_read32(&ide, PCI_BAR4);

    if ((bar & PCI_BAR_IO) == 0) {
        return 0;
    }

    /* The window is sixteen bytes - a command and a status byte, then the
     * address of the table, for each of the two channels - so the four low
     * bits of the register are the alignment and not part of the address. */
    port = (k_u16_t)(bar & ~0x0Fu);

    if (port == 0) {
        return 0;
    }

    port_x86_64_pci_enable(&ide, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);

    /* The window has to answer what is written to it: a function that is not
     * there answers all ones, and the address of the table is the one
     * register whose value the driver can predict. */
    port_outl(port + ATA_BM_PRDT, 0x1000u);

    if (port_inl(port + ATA_BM_PRDT) != 0x1000u) {
        port_outl(port + ATA_BM_PRDT, 0);
        return 0;
    }

    port_outl(port + ATA_BM_PRDT, 0);

    return port;
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
    bool dma_capable = false;
    int partitions;

    if (!ata_identify(&g_medium_sectors, &dma_capable)) {
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
     * The bus master, if the machine and the drive have one (ADR 0032): the
     * line of the channel is opened, the handler is put in the table, and the
     * registers of the function are kept for the transfers. A machine that
     * says no keeps the PIO path of 1.5.0 and says so in the log.
     */
    if (dma_capable) {
        g_dma_io = ata_dma_probe();
    }

    if (g_dma_io != 0) {
        kinfo("ata0: bus master on 0x%04x, IRQ %u after the table of the interrupts",
              (unsigned)g_dma_io, (unsigned)IRQ_ATA);
    } else {
        kinfo("ata0: PIO, %s", dma_capable ? "no bus master" : "the drive has no DMA");
    }

    /*
     * The platter is the slowest thing the machine has, and a filesystem
     * touches the metadata of a volume again for every block it allocates:
     * the block layer keeps the last sectors of the disk between the
     * filesystem and the port, and holds the writes back until a sync or
     * until the cache needs the room - a run of sectors then leaves it as
     * one command of the drive. The filesystems of this machine sync when
     * they close a file and when they unmount, so what a call returned
     * survives the machine.
     *
     * The cache of a disk is larger than the sixteen sectors of the default:
     * a bus master moves 64 KiB in a command, and a cache that holds a whole
     * run of the medium is what turns the writes of a file into those
     * commands. It is the RAM of a machine with megabytes, and not the
     * hundred kilobytes of the board (which keeps the default).
     */
    if (k_blockdev_register_cached_sized(&g_ata_device, true, ATA_CACHE_SECTORS) != 0) {
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
 * The line of the channel, opened after the port has built the table of the
 * interrupts and programmed the 8259: a driver that asks for its vector and
 * its mask earlier is asking a machine that will overwrite both (the table
 * starts as "nobody expects this" and the mask starts as "nothing is open"),
 * and the disk would fall back to a poll of the status without saying so.
 */
void port_x86_64_disk_irq_init(void)
{
    if (g_dma_io == 0) {
        return;
    }

    port_x86_64_idt_set(IRQ_BASE + IRQ_ATA, isr_irq_ata);
    port_x86_64_irq_unmask(IRQ_ATA);

    g_dma_live = true;

    kinfo("ata0: bus-master DMA on 0x%04x, IRQ %u", (unsigned)g_dma_io, (unsigned)IRQ_ATA);
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
