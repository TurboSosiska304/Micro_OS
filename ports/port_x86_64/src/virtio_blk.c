/*
 * Micro OS — the x86-64 port: the disk of the machine as a virtio device.
 *
 * The volume of the machine was a drive on the legacy IDE channel until 1.6
 * (src/ata.c, ADR 0008): a register file of the port space, a table of
 * descriptors in the memory for the bus master, and a command of at most 128
 * sectors of 512 bytes that the controller moves at a time (ADR 0032). This
 * file is the second disk of the port - the other one, because a machine can
 * have both - and it speaks virtio-blk in the legacy interface of the
 * specification: the I/O window of src/virtio.c, one split virtqueue, and a
 * request that is a chain of descriptors over the memory of the caller.
 *
 * What virtio gives a disk that the IDE channel does not, in the order the
 * driver feels it:
 *
 *   * a request names its buffers, so nothing is cut at 64 KiB and nothing is
 *     gathered in a buffer of the driver: the sixteen pages of a run of 64
 *     KiB, the two sectors of a block of the filesystem and the 512 sectors
 *     of the cache of the block layer are all the same chain, and a request
 *     as long as the queue allows is one command of the device;
 *   * the completion is an entry of the used ring and a byte of status the
 *     device writes, not a mode of the channel and a status register of the
 *     drive to be read and cleared;
 *   * the line of the card is an ordinary PCI interrupt and the driver waits
 *     for it with the processor halted (the pattern of ADR 0032), instead of
 *     polling a register with the interrupts off.
 *
 * The contract of the block layer does not change: this file registers a
 * struct k_blockdev with the operations of kernel/blockdev.h, and the
 * filesystems above it - MicroFS, ext2, FAT - do not know that the disk under
 * them is not the channel of 1984. The sector of the device is the block of
 * MicroFS, as on the ATA port, because the unit of the block layer is the
 * unit the filesystems were written in; the two sectors of 512 bytes of the
 * medium that make one of them are a detail of this file.
 *
 * The address of a buffer is translated per page, exactly as ADR 0032 says
 * for the bus master of the IDE channel: the driver is handed the buffer of
 * the caller, which may be a page of a program, and the page tables are the
 * only place that knows where its frame is. A page that does not translate
 * ends the request with an error; there is no PIO path here to fall back to,
 * and the block layer reports the failure to the caller.
 *
 * The device is optional. A machine without the PCI function (the default of
 * `make x86-run`, and every boot of the release before this one) registers no
 * disk and the boot mounts the partition of its IDE drive exactly as it did,
 * so the two disks of the port are the same system with one more device in it.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/partition.h"
#include "kernel/port/time.h"

#include "mfs/mfs.h"

#include "port_x86_64_pci.h"
#include "port_x86_64_virtio.h"

/* --- the card and the device ---------------------------------------------- */

#define PCI_VENDOR_REDHAT 0x1AF4u
#define PCI_DEVICE_VIRTIO_BLK_LEGACY 0x1001u
#define PCI_DEVICE_VIRTIO_BLK_MODERN 0x1042u

/* The one queue of a block device: every request of the disk goes through it,
 * and the device has no second one. */
#define VBLK_QUEUE 0u

/* The feature the driver takes when the device has it: a flush of the cache of
 * the device, which is what a filesystem asks for when it closes a file. */
#define VIRTIO_BLK_F_FLUSH (1u << 9)

/* The types of a request, in the order of the specification. */
#define VIRTIO_BLK_T_IN 0u    /* the device fills the buffers: a read */
#define VIRTIO_BLK_T_OUT 1u   /* the device reads them: a write */
#define VIRTIO_BLK_T_FLUSH 4u /* the cache of the device to the medium */

/* The sector of the medium and the block of the filesystem, as in src/ata.c:
 * the device is registered in the unit the filesystems speak. */
#define VBLK_MEDIUM_SECTOR 512u
#define VBLK_SECTORS_PER_BLOCK (MFS_BLOCK_SIZE / VBLK_MEDIUM_SECTOR)
#define VBLK_PAGE 4096u

_Static_assert(MFS_BLOCK_SIZE % VBLK_MEDIUM_SECTOR == 0,
               "a block of the filesystem is not a whole number of disk sectors");

/*
 * The cache of the block layer above this disk: 512 sectors of the
 * filesystem, half a megabyte, which is a whole run of a file written a
 * sector at a time and the metadata of its volume. It is the number of the
 * ATA driver for the same reason (ADR 0032): the device wants runs, and the
 * cache is what makes one out of the calls of a filesystem.
 */
#define VBLK_CACHE_SECTORS 512u

/* The run a read ahead of a file asks of the device: 128 sectors of the cache,
 * a command of the queue of 128 KiB, which is a hundred descriptors of it. */
#define VBLK_READ_AHEAD 128u

/*
 * The number of requests the driver keeps in flight at once. The queue of the
 * device is 128 or 256 entries, and a request is a header, the descriptors of
 * its pages and a byte of status: eight requests of a run are a hundred of
 * them, which is what makes one call of a filesystem a batch of the device
 * and not a command per sector.
 */
#define VBLK_INFLIGHT 8u

/*
 * A device that does not answer must not hang the machine: every wait gives
 * up after this much time, and a machine whose processor the port could not
 * calibrate gives up after this many polls instead.
 */
#define VBLK_TIMEOUT_MS 5000u
#define VBLK_FALLBACK_POLLS 50000000u

/* A request that fails is brought back once: the device is reset, its queue is
 * programmed again and the command is repeated. A disk that answers the second
 * time is a disk the caller never hears about (the pattern of ATA_ATTEMPTS in
 * src/ata.c). */
#define VBLK_ATTEMPTS 2u

/* --- the interrupt of the card -------------------------------------------- */

/* The line of the card is a line of the PCI bus and not a fixed one: the
 * configuration space names it at the byte 0x3C, and the port opens that line
 * of the 8259 the way it opens the one of the ATA channel. */
#define IRQ_BASE 0x20u
#define PCI_INTERRUPT_LINE 0x3Cu
#define PCI_IRQ_NONE 0xFFu

#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u

/* The stubs of the two vectors, in interrupts.S, and the C bodies of them. */
extern void isr_irq_virtio_blk(void);
void port_x86_64_irq_virtio_blk(k_u64_t *frame);

/* --- the state of the driver ---------------------------------------------- */

/*
 * The header of a request, as the device reads it: the type, the priority
 * nobody uses and the first sector of the medium. The numbers are 32, 32 and
 * 64 bits and the structure is the one of the specification.
 */
struct vblk_header {
    k_u32_t type;
    k_u32_t ioprio;
    k_u64_t sector;
};

static k_u16_t g_vblk_base; /* the first port of the I/O BAR, zero when there is no card */
static struct port_x86_64_pci_device g_vblk_card; /* the function of the card, for its line */
static k_u8_t g_vblk_line = PCI_IRQ_NONE;         /* the line of the 8259 the card raises */
static bool g_vblk_irq;                           /* the line of the card is open */
static bool g_vblk_flush;                         /* the device has a cache and takes a flush */
static struct virtqueue g_vblk_queue;

/*
 * The requests in flight. Each one has a header and a byte of status of its
 * own, in the memory of the kernel that the device reads and writes; the slot
 * of a request is the order it was given to the device in, so the driver needs
 * no table from a descriptor to a request - the used ring answers in the order
 * the device took them, and the head of the chain is checked against it.
 */
static struct vblk_header g_vblk_header[VBLK_INFLIGHT] __attribute__((aligned(8)));
static k_u8_t g_vblk_status[VBLK_INFLIGHT];

static k_u16_t g_vblk_head[VBLK_INFLIGHT]; /* the first descriptor of each request */
static k_u16_t g_vblk_first;               /* the slot of the oldest request in flight */
static k_u16_t g_vblk_inflight;            /* the requests the device has of the driver */
static k_u16_t g_vblk_desc;                /* the descriptor the next request starts at */
static k_u16_t g_vblk_used;                /* the used entry the driver has reaped up to */

static bool g_vblk_warned; /* the refusal of a buffer is in the log */

static k_mutex_t g_vblk_lock = K_MUTEX_INITIALIZER;

/* The medium and the volume, as the block layer sees them. */
static k_u32_t g_medium_sectors; /* the sectors of 512 bytes of the device */
static k_u32_t g_sectors;        /* the blocks of the filesystem on it */
static const char *g_root;       /* the partition the boot mounts, if any */

/* --- the ring and the requests -------------------------------------------- */

/* The budget of a wait, in the ticks of the time stamp counter; zero when the
 * port could not measure the processor. */
static k_u64_t vblk_timeout_ticks(void)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info->cpu_hz == 0) {
        return 0;
    }

    return info->cpu_hz / 1000u * VBLK_TIMEOUT_MS;
}

/*
 * The physical address behind a byte the request names (ADR 0032 §1). The
 * window of the programs is the only memory of this machine whose virtual
 * address is not its physical one, and the page tables of the moment are the
 * only place that knows where its frame is. Everything else is arithmetic:
 * the heap is the identity map and the image is a fixed offset.
 *
 * A page that is not there answers zero, which no address of a buffer can be:
 * the request is refused instead of handing the device a number that names
 * something else.
 */
static k_u64_t vblk_phys(k_u64_t address)
{
    if (address >= PORT_X86_64_USER_BASE && address < PORT_X86_64_USER_LIMIT) {
        void *frame = port_x86_64_paging_phys(port_x86_64_current_cr3(),
                                              address & ~(k_u64_t)(VBLK_PAGE - 1u));

        if (frame == NULL) {
            return 0;
        }

        return (k_u64_t)(uintptr_t)frame | (address & (VBLK_PAGE - 1u));
    }

    return port_x86_64_bus_phys((const void *)(uintptr_t)address);
}

/* The descriptors of the ring the driver has not given to the device yet. */
static k_u16_t vblk_descriptors_left(void)
{
    return (k_u16_t)(g_vblk_queue.size - g_vblk_desc);
}

/* Forget every request the driver is waiting for: the ring is the driver's
 * again. It is what the recovery below does before the device is reset, when
 * the descriptor of a request that never finished is not worth keeping. */
static void vblk_forget(void)
{
    g_vblk_first = 0;
    g_vblk_inflight = 0;
    g_vblk_desc = 0;
    g_vblk_used = 0;
}

/*
 * Give the device one request: the header, the pieces of the buffer and the
 * byte of status, as one chain of descriptors, and the chain to the available
 * ring. Answers the number of bytes the chain describes, zero when the buffer
 * may not be handed to the device.
 *
 * A piece is cut at the page boundary and translated on its own (ADR 0032 §2),
 * so a request of a megabyte is a chain of the pages of the buffer; a chain
 * never uses more than the free descriptors of the ring, and the caller asks
 * vblk_descriptors_left() before it decides how much to hand over. A request
 * without a buffer - the flush of a cache - is a header and a status.
 *
 * The answer is the number of bytes the chain describes, and zero is a request
 * that was not given to the device. A flush describes no bytes and is still a
 * request: the caller of this function is told about it by the count of the
 * descriptors the chain used, which is zero only for a failure.
 */
static k_u16_t vblk_submit(k_u32_t type, k_u32_t lba, const k_u8_t *buf, k_size_t bytes,
                           k_u16_t flags, k_size_t *described_out)
{
    k_u16_t slot = (k_u16_t)((g_vblk_first + g_vblk_inflight) % VBLK_INFLIGHT);
    k_u16_t head = g_vblk_desc;
    k_u16_t cursor = (k_u16_t)(head + 1u);
    k_u16_t limit;
    k_size_t done = 0;
    k_size_t described = 0;

    *described_out = 0;

    if (vblk_descriptors_left() < 2u) {
        return 0;
    }

    /* The header and the byte of status are two descriptors of the chain. */
    limit = (k_u16_t)(vblk_descriptors_left() - 2u);

    g_vblk_header[slot].type = type;
    g_vblk_header[slot].ioprio = 0;
    g_vblk_header[slot].sector = lba;
    g_vblk_status[slot] = 0xFFu; /* a status the device did not write is a failure */

    while (done < bytes && described < (k_size_t)limit) {
        k_u64_t address = (k_u64_t)(uintptr_t)(buf + done);
        k_size_t left = bytes - done;
        k_size_t inside = (k_size_t)(address & (VBLK_PAGE - 1u));
        k_size_t piece = VBLK_PAGE - inside;
        k_u64_t physical;

        if (piece > left) {
            piece = left;
        }

        physical = vblk_phys(address);

        if (physical == 0 || physical > 0xFFFFFFFFu) {
            return 0;
        }

        virtqueue_descriptor(&g_vblk_queue, cursor, physical, (k_u32_t)piece,
                             (k_u16_t)(flags | VIRTQ_DESC_F_NEXT), (k_u16_t)(cursor + 1u));

        cursor++;
        described++;
        done += piece;
    }

    /* The device writes the byte of status at the end of the chain, and the
     * header is the descriptor the used ring names. */
    virtqueue_descriptor(&g_vblk_queue, cursor, vblk_phys((k_u64_t)(uintptr_t)&g_vblk_status[slot]),
                         1, VIRTQ_DESC_F_WRITE, 0);
    virtqueue_descriptor(&g_vblk_queue, head, vblk_phys((k_u64_t)(uintptr_t)&g_vblk_header[slot]),
                         (k_u32_t)sizeof(g_vblk_header[slot]), VIRTQ_DESC_F_NEXT,
                         (k_u16_t)(head + 1u));

    g_vblk_head[slot] = head;
    g_vblk_desc = (k_u16_t)(cursor + 1u);
    g_vblk_inflight++;

    virtqueue_avail_push(&g_vblk_queue, head);

    *described_out = done;

    return (k_u16_t)(g_vblk_desc - head);
}

static void vblk_recover(void);

/*
 * Wait for every request of the driver: the used ring is the answer of the
 * device, and the byte of status of each request is the answer of the medium.
 * The core halts with the interrupts on when the line of the card is open, so
 * the interrupt of the device wakes it at once and the tick of the timer - a
 * millisecond later at the most - is what keeps a machine that never raises
 * the line from sleeping for ever. A driver that cannot sleep (the interrupts
 * were off when it was called) spins with a pause instead.
 */
static int vblk_complete(bool can_sleep)
{
    k_u64_t budget;
    k_u64_t start;
    k_u32_t polls = 0;
    int rc = 0;

    if (g_vblk_inflight == 0u) {
        return 0;
    }

    virtio_barrier();
    virtio_notify(g_vblk_base, VBLK_QUEUE);

    budget = vblk_timeout_ticks();
    start = port_rdtsc();

    for (;;) {
        k_u16_t used = virtqueue_used_index(&g_vblk_queue);

        while (g_vblk_used != used) {
            k_u16_t slot = g_vblk_first;
            k_u32_t id;
            k_u32_t length;

            virtqueue_used_entry(&g_vblk_queue, g_vblk_used, &id, &length);
            g_vblk_used++;

            if (id != (k_u32_t)g_vblk_head[slot] || g_vblk_status[slot] != 0u) {
                rc = -1;
            }

            g_vblk_first = (k_u16_t)((slot + 1u) % VBLK_INFLIGHT);
            g_vblk_inflight--;
        }

        if (g_vblk_inflight == 0u) {
            break;
        }

        if (budget != 0u) {
            if (port_rdtsc() - start > budget) {
                rc = -1;
                break;
            }
        } else if (++polls >= VBLK_FALLBACK_POLLS) {
            rc = -1;
            break;
        }

        if (can_sleep && g_vblk_irq) {
            /* The processor does not take an interrupt between `sti` and the
             * instruction after it, so a completion that arrives between the
             * read of the ring above and the `hlt` below is not missed. */
            __asm__ volatile("sti; hlt" : : : "memory");
        } else {
            __asm__ volatile("pause");
        }
    }

    if (rc != 0) {
        /* A ring that still holds a request of a device that did not answer is
         * not a ring this driver can use again: the device is reset and its
         * queue is programmed from the first descriptor. */
        vblk_forget();
        vblk_recover();
    } else {
        g_vblk_desc = 0;
    }

    /*
     * The interrupt status of a device that completed while nobody was
     * waiting for its line is read and cleared here: the line of the card is
     * level-triggered on the bus, and a bit left set is a line that stays up
     * and an interrupt that arrives for a request that is already done.
     */
    if (!(can_sleep && g_vblk_irq)) {
        (void)virtio_in8(g_vblk_base, VIRTIO_PCI_ISR);
    }

    return rc;
}

/*
 * The transfer of one call of the block layer: `sectors` sectors of the medium
 * at `lba`, as many requests of the device as the ring holds. The requests of
 * one call go to the device together - one notification of the queue - and the
 * wait for them is one wait, which is what makes a run of the cache a batch
 * and not a command per sector.
 */
static int vblk_transfer(bool write, k_u32_t lba, const k_u8_t *buf, k_size_t sectors,
                         bool can_sleep)
{
    k_u32_t type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    k_u16_t flags = write ? 0u : VIRTQ_DESC_F_WRITE;
    k_size_t bytes = sectors * VBLK_MEDIUM_SECTOR;

    while (bytes > 0) {
        k_size_t described;

        if (vblk_descriptors_left() < 3u || g_vblk_inflight >= VBLK_INFLIGHT) {
            if (vblk_complete(can_sleep) != 0) {
                return -1;
            }

            continue;
        }

        if (vblk_submit(type, lba, buf, bytes, flags, &described) == 0) {
            /* The buffer has a page this machine cannot name - a page that is
             * not there, or a frame above the four gigabytes the descriptor of
             * a legacy device holds - and the request is refused. One line,
             * the first time: a filesystem hands the driver the buffer of a
             * program on every call. */
            if (!g_vblk_warned) {
                g_vblk_warned = true;
                kwarn("virtio0: the %s at %u names a page the device cannot read",
                      write ? "write" : "read", (unsigned)lba);
            }

            (void)vblk_complete(can_sleep);
            return -1;
        }

        lba += (k_u32_t)(described / VBLK_MEDIUM_SECTOR);
        buf += described;
        bytes -= described;
    }

    return vblk_complete(can_sleep);
}

/* --- the bring-up of the device -------------------------------------------- */

static bool vblk_start(void)
{
    k_u32_t features;
    k_u32_t wanted;

    if (!virtio_reset(g_vblk_base)) {
        kerror("virtio0: the device did not take a reset");
        return false;
    }

    virtio_status(g_vblk_base, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_status(g_vblk_base, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    features = virtio_in32(g_vblk_base, VIRTIO_PCI_DEVICE_FEATURES);
    wanted = features & VIRTIO_BLK_F_FLUSH;

    virtio_out32(g_vblk_base, VIRTIO_PCI_DRIVER_FEATURES, wanted);
    g_vblk_flush = (wanted & VIRTIO_BLK_F_FLUSH) != 0u;

    if (!virtio_queue_setup(g_vblk_base, &g_vblk_queue, VBLK_QUEUE, true)) {
        kerror("virtio0: the queue of the device is not usable");
        virtio_status(g_vblk_base, VIRTIO_STATUS_FAILED);
        return false;
    }

    virtio_barrier();
    virtio_status(g_vblk_base,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    return true;
}

/*
 * The device that did not answer is brought back: the rings are empty, the
 * device is reset and its queue is programmed again from the first byte. The
 * caller repeats the command once, and what a repeated command cannot fix is
 * an error the filesystem above reports to the program that asked for it.
 */
static void vblk_recover(void)
{
    if (!vblk_start()) {
        kerror("virtio0: the device did not come back, the disk is gone");
    }
}

/* --- the device of the block layer ---------------------------------------- */

/*
 * One call of the block layer, as the device takes it: the two attempts of a
 * command that failed, and the mutex that keeps two tasks off one queue. The
 * mutex is taken only when the interrupts of the moment are on: a caller that
 * comes with them off is inside a section of the kernel that no other task can
 * enter, and taking a mutex there is a deadlock and not a lock. The block
 * layer's own mutex serialises the calls of a cached device anyway; this one
 * is what a device without a cache above it needs.
 */
static int vblk_xfer(bool write, k_u32_t lba, const void *buf, k_size_t sectors)
{
    k_u64_t flags = port_save_flags_and_cli();
    bool live = (flags & 0x200u) != 0; /* the interrupts of the moment */
    k_u32_t attempt;
    int rc = -1;

    port_restore_flags(flags);

    if (live && k_mutex_lock(&g_vblk_lock) != 0) {
        return -1;
    }

    for (attempt = 0; attempt < VBLK_ATTEMPTS; attempt++) {
        rc = vblk_transfer(write, lba, (const k_u8_t *)buf, sectors, live);

        if (rc == 0) {
            break;
        }
    }

    if (live) {
        k_mutex_unlock(&g_vblk_lock);
    }

    if (rc != 0) {
        kerror("virtio0: the %s of %u sector(s) at %u failed", write ? "write" : "read",
               (unsigned)sectors, (unsigned)lba);
    }

    return rc;
}

static bool vblk_range_ok(k_u32_t sector, k_size_t count)
{
    return g_sectors != 0 && sector < g_sectors && count <= (k_size_t)g_sectors - sector;
}

static int vblk_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !vblk_range_ok(sector, count)) {
        return -1;
    }

    return vblk_xfer(false, sector * VBLK_SECTORS_PER_BLOCK, (const k_u8_t *)buf,
                     count * VBLK_SECTORS_PER_BLOCK);
}

static int vblk_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (buf == NULL || !vblk_range_ok(sector, count)) {
        return -1;
    }

    return vblk_xfer(true, sector * VBLK_SECTORS_PER_BLOCK, (const k_u8_t *)buf,
                     count * VBLK_SECTORS_PER_BLOCK);
}

/*
 * The device has a cache of its own: the writes of a file collect in it and
 * the flush command hands them to the medium. The filesystem calls this when
 * it closes a file, makes a directory or unmounts the volume, so the data of
 * a call that returned is on the medium even if the machine stops.
 *
 * A device that did not offer the feature has no cache to flush and the call
 * is a no-op of this driver - and the device wrote through, which is what the
 * absence of the feature means.
 */
static int vblk_sync(void *ctx)
{
    k_u64_t flags;
    bool live;
    k_u32_t attempt;
    k_size_t described;
    int rc = -1;

    K_UNUSED(ctx);

    if (!g_vblk_flush) {
        return 0;
    }

    flags = port_save_flags_and_cli();
    live = (flags & 0x200u) != 0;
    port_restore_flags(flags);

    if (live && k_mutex_lock(&g_vblk_lock) != 0) {
        return -1;
    }

    for (attempt = 0; attempt < VBLK_ATTEMPTS; attempt++) {
        if (vblk_submit(VIRTIO_BLK_T_FLUSH, 0, NULL, 0, 0, &described) != 0 &&
            vblk_complete(live) == 0) {
            rc = 0;
            break;
        }
    }

    if (live) {
        k_mutex_unlock(&g_vblk_lock);
    }

    if (rc != 0) {
        kerror("virtio0: the flush of the device failed");
    }

    return rc;
}

static const struct k_blockdev_ops g_vblk_ops = {
    .read = vblk_read,
    .write = vblk_write,
    .erase = NULL, /* a disk keeps its sectors until they are written */
    .sync = vblk_sync,
};

static struct k_blockdev g_vblk_device = {
    .name = "virtio0",
    .sector_size = MFS_BLOCK_SIZE,
    .sectors = 0, /* the size of the medium, read by the configuration below */
    .ops = &g_vblk_ops,
    .ctx = NULL,
    .read_ahead = VBLK_READ_AHEAD,
};

/* --- the boot ------------------------------------------------------------- */

/*
 * The card of the machine: the PCI function of the vendor of QEMU whose device
 * id is the disk of the legacy interface, its first BAR as an I/O window, and
 * the two switches of the command register - the window and the bus master.
 *
 * A machine with a modern-only card is named instead of silently left without
 * a disk: the driver speaks the legacy window, and a card that has none would
 * answer "no" for a reason nobody could see (the same sentence of
 * src/virtio_net.c).
 */
static bool vblk_probe(struct port_x86_64_pci_device *card)
{
    k_u32_t bar0;

    if (port_x86_64_pci_find(PCI_VENDOR_REDHAT, PCI_DEVICE_VIRTIO_BLK_MODERN, NULL)) {
        kerror("virtio0: the machine offers only the modern virtio "
               "interface, which this driver does not speak");
        return false;
    }

    if (!port_x86_64_pci_find(PCI_VENDOR_REDHAT, PCI_DEVICE_VIRTIO_BLK_LEGACY, card)) {
        return false;
    }

    bar0 = port_x86_64_pci_read32(card, PCI_BAR0);

    if ((bar0 & 1u) == 0u) {
        kerror("virtio0: the legacy BAR is not an I/O window");
        return false;
    }

    g_vblk_base = (k_u16_t)(bar0 & ~3u);
    g_vblk_card = *card;

    port_x86_64_pci_enable(card, PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    return true;
}

/*
 * The capacity of the medium: the first eight bytes of the configuration of
 * the device, in sectors of 512 bytes. The first word is the whole capacity of
 * every image this machine boots (two terabytes) and the second is counted
 * only to be refused: the device of the port addresses the medium with 32 bits
 * of sectors, as the ATA driver addresses its drive with LBA28.
 */
static k_u32_t vblk_capacity(void)
{
    k_u32_t low = virtio_in32(g_vblk_base, (k_u16_t)(VIRTIO_PCI_CONFIG + 0u));
    k_u32_t high = virtio_in32(g_vblk_base, (k_u16_t)(VIRTIO_PCI_CONFIG + 4u));

    if (high != 0u) {
        return 0xFFFFFFFFu;
    }

    return low;
}

/*
 * The disk of the port, if QEMU was given one on the virtio bus. The root
 * volume of the boot is the first partition of it, exactly as on the ATA
 * channel (ADR 0008): the whole disk is not a volume, and the boot says so
 * when the table is missing instead of formatting over it.
 */
void port_x86_64_virtio_blk_init(void)
{
    struct port_x86_64_pci_device card;
    const struct k_blockdev *disk;
    const char *first;
    int partitions;

    if (!vblk_probe(&card)) {
        return;
    }

    if (!vblk_start()) {
        g_vblk_base = 0;
        return;
    }

    g_medium_sectors = vblk_capacity();

    if (g_medium_sectors < VBLK_SECTORS_PER_BLOCK ||
        (g_medium_sectors % VBLK_SECTORS_PER_BLOCK) != 0) {
        kerror("virtio0: the medium is not a whole number of %u-byte blocks",
               (unsigned)MFS_BLOCK_SIZE);
        g_vblk_base = 0;
        return;
    }

    g_sectors = g_medium_sectors / VBLK_SECTORS_PER_BLOCK;
    g_vblk_device.sectors = g_sectors;

    /*
     * The cache of the block layer: a run of the medium is one request of the
     * device, and the writes of a filesystem collect in the cache until a sync
     * or until the cache needs the room.
     */
    if (k_blockdev_register_cached_sized(&g_vblk_device, true, VBLK_CACHE_SECTORS) != 0) {
        kerror("virtio0: cannot register the disk");
        g_vblk_base = 0;
        return;
    }

    kinfo("virtio0: %u MB of virtio-blk, %u descriptor(s), %s the flush",
          (unsigned)(g_medium_sectors / 2048u), (unsigned)g_vblk_queue.size,
          g_vblk_flush ? "with" : "without");

    /*
     * The scan reads through the device the table hands out, not through the
     * structure of the platform: that is the view with the cache, and the
     * partitions keep the parent they were found on.
     */
    disk = k_blockdev_find(g_vblk_device.name);

    if (disk == NULL) {
        disk = &g_vblk_device;
    }

    partitions = partition_scan(disk);

    if (partitions == KERR_NOENT) {
        kinfo("virtio0: no partition table, the disk is not used");
        return;
    }

    if (partitions < 0) {
        kerror("virtio0: the partition table is not usable (%d)", partitions);
        return;
    }

    first = partition_name(disk, 0);

    if (first == NULL) {
        return;
    }

    g_root = first;
    kinfo("x86-64: the root volume is %s (%d partition(s))", first, partitions);
}

/* The first partition of the virtio disk, NULL when the machine has no such
 * disk: the boot prefers it to the drive of the IDE channel when both are
 * there, because it is the device of this release. */
const char *port_x86_64_virtio_blk_root(void)
{
    return g_root;
}

/*
 * The line of the card, opened after the port has built the table of the
 * interrupts and programmed the 8259: the same rule as the one of the ATA
 * channel. The number of the line is the device's - the configuration space
 * names the line the firmware gave it, which QEMU routes through the 8259 -
 * and a line the driver cannot open leaves the queue polled and says so.
 */
void port_x86_64_virtio_blk_irq_init(void)
{
    k_u8_t line;

    if (g_vblk_base == 0u) {
        return;
    }

    line = (k_u8_t)(port_x86_64_pci_read16(&g_vblk_card, PCI_INTERRUPT_LINE) & 0xFFu);

    if (line == PCI_IRQ_NONE || line >= 16u) {
        kinfo("virtio0: the card names no line of the 8259, the queue is polled");
        return;
    }

    /*
     * The status of the device is read before the line is opened: a request
     * that completed while the driver was polling left the line of the card
     * asserted, and the edge of a level-triggered line is delivered the moment
     * the 8259 stops masking it.
     */
    (void)virtio_in8(g_vblk_base, VIRTIO_PCI_ISR);

    g_vblk_line = line;
    g_vblk_irq = true;

    port_x86_64_idt_set(IRQ_BASE + line, isr_irq_virtio_blk);

    port_x86_64_irq_unmask(line);

    kinfo("virtio0: IRQ %u and a queue of %u descriptor(s)", (unsigned)line,
          (unsigned)g_vblk_queue.size);
}

/*
 * The interrupt of the card: the read of the interrupt status is what takes
 * the line down, and the 8259 is told that the interrupt is over.
 *
 * A line of the PCI bus is level-triggered (the ELCR of the PIIX says so) and
 * the interrupt of a slave is a level of the cascade of the master, so the
 * order of the three steps is not free: the line is masked, the two chips are
 * acknowledged, and the line is opened again. A chip that is acknowledged
 * while its line is still high latches the request again between the EOI of
 * the slave and the EOI of the master, and what the processor takes then is
 * the vector of the cascade with nothing behind it - the "spurious IRQ" of the
 * PC, which on this port is an interrupt no handler owns and a machine that
 * stops. The window is the one the interrupt status of the device closes and a
 * completion that arrives a moment later opens again; a device with a queue of
 * 256 descriptors has one of those every few microseconds.
 *
 * The handler does not touch the rings: the used entry the device wrote is the
 * answer of the wait of the driver, and reading the ring here would hide it
 * from that wait.
 */
void port_x86_64_irq_virtio_blk(k_u64_t *frame)
{
    K_UNUSED(frame);

    (void)virtio_in8(g_vblk_base, VIRTIO_PCI_ISR);

    if (g_vblk_line < 8u) {
        k_u8_t mask = port_inb(PIC1_DATA);

        port_outb(PIC1_DATA, (k_u8_t)(mask | (k_u8_t)(1u << g_vblk_line)));
        port_outb(PIC1_COMMAND, PIC_EOI);
        port_outb(PIC1_DATA, mask);
    } else if (g_vblk_line < 16u) {
        k_u8_t mask = port_inb(PIC2_DATA);

        port_outb(PIC2_DATA, (k_u8_t)(mask | (k_u8_t)(1u << (g_vblk_line - 8u))));
        port_outb(PIC2_COMMAND, PIC_EOI);
        port_outb(PIC1_COMMAND, PIC_EOI);
        port_outb(PIC2_DATA, mask);
    }
}
