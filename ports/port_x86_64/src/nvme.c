/*
 * Micro OS — the x86-64 port: the disk of a modern laptop, on NVMe (ADR 0042).
 *
 * The SATA controller of ADR 0041 is the disk of a machine of the transition;
 * the disk of a laptop of these years is behind a controller that has no
 * channels and no command list of a port: NVMe is a function of the PCI bus
 * (class 01, sub-class 08, 64-bit BAR0 in the memory) with two doorbells and
 * a pair of rings for every queue. The host writes a command of 64 bytes into
 * a submission queue and rings the tail doorbell; the controller writes a
 * completion of 16 bytes into the completion queue and sets the phase bit;
 * the host reads it, moves the head and rings the other doorbell.
 *
 * The driver speaks the protocol the release needs: the admin queue is
 * created by the boot (AQA, ASQ, ACQ, CC.EN, CSTS.RDY), Identify Controller
 * and Identify Namespace say who the drive is and how many blocks of 512
 * bytes it has, and one I/O queue pair is created through the admin queue.
 * READ (0x02) and WRITE (0x01) move the bytes, with PRP1 and PRP2 - the two
 * physical pages a request of at most two pages names - which is the
 * contract of ADR 0032 again: the buffer of the caller, one address per
 * page, translated by the page tables of the program that owns it. Flush
 * (0x00) brings the cache of the namespace to the medium at a sync.
 *
 * The disk is **read-only until a mount asks**, by the rule of ADR 0041: the
 * header block of MicroFS and a name in /etc/volumes decide, the door of the
 * writes starts closed, and a driver whose door is closed has no way to build
 * a WRITE command. The first boot with a controller lists what it found.
 *
 * The completion is polled, as the port polls the port of AHCI: the interrupt
 * of the controller (MSI-X) is a line and a table of vectors that this step
 * does not open, and one disk of a boot is a read. It is a debt of the lane
 * in docs/debts.md, with the many queues and the hot-plug of the medium.
 *
 * The device is optional: a machine without the function registers nothing
 * here and boots the medium it had.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/partition.h"
#include "kernel/volume.h"

#include "mfs/mfs.h"

#include "port_x86_64_nvme.h"
#include "port_x86_64_pci.h"

/* --- the function on the PCI bus ------------------------------------------ */

#define PCI_CLASS_STORAGE 0x01u
#define PCI_SUBCLASS_NVME 0x08u

#define PCI_BAR0 0x10u
#define PCI_BAR1 0x14u /* the upper half of the 64-bit window of BAR0 */
#define PCI_BAR_IO 0x00000001u
#define PCI_BAR_TYPE_MASK 0x06u
#define PCI_BAR_TYPE_64 0x04u
#define PCI_BAR_MEMORY_MASK 0x0Fu

/* --- the registers of the controller -------------------------------------- */

#define NVME_CAP 0x00u  /* the capabilities: queues, stride, command sets */
#define NVME_VS 0x08u   /* the version of the specification */
#define NVME_CC 0x14u   /* enable, page size, entry sizes */
#define NVME_CSTS 0x1Cu /* ready, fatal status, shutdown */
#define NVME_AQA 0x24u  /* the sizes of the admin queues */
#define NVME_ASQ 0x28u  /* the admin submission queue, its physical address */
#define NVME_ACQ 0x30u  /* the admin completion queue */
#define NVME_DOORBELL 0x1000u

/* CAP: the low word is MQES (the largest queue, minus one), and the stride of
 * the doorbells is a field of the high word. */
#define NVME_CAP_MQES(cap) ((k_u32_t)((cap) & 0xFFFFu))
#define NVME_CAP_DSTRD(cap) ((k_u32_t)(((cap) >> 32) & 0x0Fu))
#define NVME_CAP_CSS(cap) ((k_u32_t)(((cap) >> 37) & 0xFFu))
#define NVME_CAP_MPSMIN(cap) ((k_u32_t)(((cap) >> 48) & 0x0Fu))

#define NVME_CSS_NVM 0x01u /* the command set of a disk: NVM */

/* CC: the controller is enabled with the two sizes of the queue entries and
 * the page size the host chooses (the smallest the controller takes). */
#define NVME_CC_EN 0x00000001u
#define NVME_CC_CSS_MASK 0x00000070u
#define NVME_CC_MPS_SHIFT 7u
#define NVME_CC_IOSQES_SHIFT 16u
#define NVME_CC_IOCQES_SHIFT 20u
#define NVME_SQ_ENTRY_SHIFT 6u /* 64 bytes */
#define NVME_CQ_ENTRY_SHIFT 4u /* 16 bytes */

#define NVME_CSTS_RDY 0x00000001u
#define NVME_CSTS_CFS 0x00000002u /* a fatal status of the controller */

/* --- the commands of the admin and of the NVM set ------------------------- */

#define NVME_ADMIN_CREATE_SQ 0x01u
#define NVME_ADMIN_CREATE_CQ 0x05u
#define NVME_ADMIN_IDENTIFY 0x06u
#define NVME_IO_FLUSH 0x00u
#define NVME_IO_WRITE 0x01u
#define NVME_IO_READ 0x02u

/* The CNS of Identify: the controller, then a namespace. */
#define NVME_IDENTIFY_CONTROLLER 0x01u
#define NVME_IDENTIFY_NAMESPACE 0x00u

/* The queue flags of Create: the queue is physically contiguous, and the
 * completion queue is polled (no interrupt vector). */
#define NVME_QUEUE_PC 0x00000001u
#define NVME_QUEUE_IEN 0x00000002u

/* The status field of a completion: the phase bit, then the code and the type
 * of the answer. Zero is success. */
#define NVME_STATUS_PHASE 0x0001u

/* --- the queues ----------------------------------------------------------- */

/*
 * The entries of the two admin queues and of the one I/O queue pair. The
 * admin queue is a handshake of a boot and 32 entries are a ceiling of the
 * protocol; the I/O queue is as large as the driver's ring and no larger than
 * the controller allows (CAP.MQES).
 */
#define NVME_ADMIN_ENTRIES 32u
#define NVME_IO_ENTRIES 64u

/* One command of 64 bytes, as the controller reads it. The fields are the
 * ones of the specification in the order they are written. */
struct nvme_command {
    k_u32_t cdw0; /* opcode, fuse, the identifier of the command */
    k_u32_t nsid;
    k_u32_t cdw2;
    k_u32_t cdw3;
    k_u32_t mptr_low; /* the metadata of a namespace that keeps it apart */
    k_u32_t mptr_high;
    k_u32_t prp1_low;
    k_u32_t prp1_high;
    k_u32_t prp2_low;
    k_u32_t prp2_high;
    k_u32_t cdw10;
    k_u32_t cdw11;
    k_u32_t cdw12;
    k_u32_t cdw13;
    k_u32_t cdw14;
    k_u32_t cdw15;
};

_Static_assert(sizeof(struct nvme_command) == 64u, "the command of NVMe changed");

/* One completion of 16 bytes. `status` carries the phase in its lowest bit
 * and the code and the type of the answer above it. */
struct nvme_completion {
    k_u32_t result;
    k_u32_t reserved;
    k_u16_t sq_head;
    k_u16_t sq_id;
    k_u16_t command;
    k_u16_t status;
};

_Static_assert(sizeof(struct nvme_completion) == 16u, "the completion of NVMe changed");

/* One queue: the rings, their doorbells and the two cursors of the host. */
struct nvme_queue {
    struct nvme_command *sq;
    struct nvme_completion *cq;
    k_u16_t entries;
    k_u16_t tail;  /* the next free slot of the submission queue */
    k_u16_t head;  /* the next completion the host has not read */
    k_u16_t phase; /* the phase the next completion carries */
    k_u16_t cid;   /* the identifier of the next command */
    k_u32_t sq_db; /* the offset of the doorbell of the submission tail */
    k_u32_t cq_db; /* the offset of the doorbell of the completion head */
};

/* The sector of the medium, read from the namespace (LBA format). The block
 * layer sees the same size the medium names: a 512-byte namespace is 512-byte
 * sectors, not the 1024-byte block of MicroFS. A partition that ends on an
 * odd 512-byte LBA is real and must be representable. */

/* A request of the driver names at most two pages (PRP1 and PRP2), which is
 * what the specification calls the "two-page" case: the first piece starts at
 * the offset of the buffer and the second is one whole page. A larger run is
 * another command of the queue, and a PRP list is the next step of the lane. */
#define NVME_PROTOCOL_PAGE 4096u
#define NVME_TRANSFER_MAX (2u * NVME_PROTOCOL_PAGE)

/* The cache of the block layer above the disk, and the run a read ahead of a
 * file asks of it (ADR 0032), in bytes: 512 KiB of cache, 128 KiB of read
 * ahead. The sector count is derived from the namespace LBA size so a 512-byte
 * namespace keeps the same byte budget as a 4096-byte one. Write-through,
 * because a write is the answer of the door of ADR 0041. */
#define NVME_CACHE_BYTES (512u * 1024u)
#define NVME_READ_AHEAD_BYTES (128u * 1024u)

/* A controller that does not answer must not hang the boot: every wait gives
 * up after this much time, and a machine whose processor the port could not
 * calibrate gives up after this many polls instead. */
#define NVME_TIMEOUT_MS 5000u
#define NVME_FALLBACK_POLLS 50000000u

/* --- the memory of the controller ----------------------------------------- */

/*
 * The rings and the buffers the controller reads and writes. All of them are
 * physical addresses, and all of them live in the image of the kernel, which
 * the port translates with port_x86_64_kernel_phys(): a ring of 4 KiB and a
 * doorbell of four bytes are the same kind of number to the device.
 */
static struct nvme_command g_admin_sq[NVME_ADMIN_ENTRIES] __attribute__((aligned(4096)));
static struct nvme_completion g_admin_cq[NVME_ADMIN_ENTRIES] __attribute__((aligned(4096)));
static struct nvme_command g_io_sq[NVME_IO_ENTRIES] __attribute__((aligned(4096)));
static struct nvme_completion g_io_cq[NVME_IO_ENTRIES] __attribute__((aligned(4096)));
static k_u8_t g_identify[4096] __attribute__((aligned(4096)));

/* --- the state of the driver ---------------------------------------------- */

static volatile k_u8_t *g_regs; /* the window of the registers */
static k_u32_t g_dstrd;         /* the stride of the doorbells, from CAP */
static k_u32_t g_page;          /* the page size the controller works in */
static k_u32_t g_nsid = 1u;     /* the namespace the boot mounts */
static k_u32_t g_lba_size;      /* the formatted LBA size of the namespace */
static k_u32_t g_sectors;       /* the namespace size, in g_lba_size units */
static const char *g_root;
static const struct k_blockdev *g_volume; /* the partition the rule is about */
static bool g_read_only = true;
static bool g_write_open;
static const char *g_refusal;
static bool g_refused;
static struct k_volume_id g_id;      /* what the rule saw on the volume */
static struct k_volume_id g_disk_id; /* the whole disk: the table of the writes */
static struct nvme_queue g_admin;
static struct nvme_queue g_io;
static k_mutex_t g_nvme_lock = K_MUTEX_INITIALIZER;

/* --- one register of the controller --------------------------------------- */

static k_u32_t reg_read32(k_u32_t offset)
{
    return *(volatile k_u32_t *)(g_regs + offset);
}

static void reg_write32(k_u32_t offset, k_u32_t value)
{
    *(volatile k_u32_t *)(g_regs + offset) = value;
}

static k_u64_t reg_read64(k_u32_t offset)
{
    k_u64_t low = reg_read32(offset);
    k_u64_t high = reg_read32(offset + 4u);

    return low | (high << 32);
}

/* The two halves of a 64-bit register are written low first: the controller
 * reads the number between the two writes only if the high half moves. */
static void reg_write64(k_u32_t offset, k_u64_t value)
{
    reg_write32(offset, (k_u32_t)value);
    reg_write32(offset + 4u, (k_u32_t)(value >> 32));
}

/* One doorbell: the offset of the register plus the queue's place in the row
 * of them (the stride of CAP.DSTRD, four bytes for the functions of QEMU). */
static void doorbell(k_u32_t offset, k_u32_t value)
{
    *(volatile k_u32_t *)(g_regs + NVME_DOORBELL + offset) = value;
}

/* The budget of a wait, in the ticks of the time stamp counter; zero when the
 * port could not measure the processor. */
static k_u64_t nvme_timeout_ticks(void)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info->cpu_hz == 0u) {
        return 0;
    }

    return info->cpu_hz / 1000u * NVME_TIMEOUT_MS;
}

/* Wait for CSTS.RDY to be the value the caller wants. */
static bool nvme_wait_ready(bool ready)
{
    k_u64_t budget = nvme_timeout_ticks();
    k_u64_t start = port_rdtsc();
    k_u32_t polls = 0;

    for (;;) {
        k_u32_t status = reg_read32(NVME_CSTS);

        if (((status & NVME_CSTS_RDY) != 0u) == ready) {
            if (ready && (status & NVME_CSTS_CFS) != 0u) {
                kerror("nvme0: the controller says it has a fatal status");
                return false;
            }

            return true;
        }

        if (budget != 0u) {
            if (port_rdtsc() - start > budget) {
                return false;
            }
        } else if (++polls >= NVME_FALLBACK_POLLS) {
            return false;
        }

        __asm__ volatile("pause");
    }
}

/* --- the submission and the completion of one command --------------------- */

/*
 * Wait for the completion of the command at the head of the queue: the phase
 * bit of the entry is the answer of the controller (it flips with every pass
 * of the ring), and the code and the type of the status are the verdict. The
 * head and the phase move only when the entry was read, so a command that
 * does not answer leaves the queue where it was for the sentence of the log.
 */
static int nvme_completion(struct nvme_queue *queue)
{
    k_u64_t budget = nvme_timeout_ticks();
    k_u64_t start = port_rdtsc();
    k_u32_t polls = 0;

    for (;;) {
        struct nvme_completion *slot = &queue->cq[queue->head];
        k_u16_t status = *(volatile k_u16_t *)&slot->status;

        if ((status & NVME_STATUS_PHASE) == (queue->phase & NVME_STATUS_PHASE)) {
            k_u32_t code = (status >> 1) & 0xFFu;
            k_u32_t type = (status >> 9) & 0x7u;

            __asm__ volatile("" ::: "memory");

            queue->head = (k_u16_t)((queue->head + 1u) % queue->entries);

            if (queue->head == 0u) {
                queue->phase ^= NVME_STATUS_PHASE;
            }

            doorbell(queue->cq_db, queue->head);

            if (code != 0u || type != 0u) {
                kwarn("nvme0: a command answered status 0x%02x of type %u", (unsigned)code,
                      (unsigned)type);
                return -1;
            }

            return 0;
        }

        if (budget != 0u) {
            if (port_rdtsc() - start > budget) {
                return -1;
            }
        } else if (++polls >= NVME_FALLBACK_POLLS) {
            return -1;
        }

        __asm__ volatile("pause");
    }
}

/*
 * One command of the queue: the identifier in CDW0, the 64 bytes into the
 * next free slot, a barrier (the controller reads the memory the doorbell
 * announces) and the tail doorbell.
 */
static int nvme_submit(struct nvme_queue *queue, struct nvme_command *command)
{
    k_u16_t cid = queue->cid;

    command->cdw0 = (command->cdw0 & 0xFFFFu) | ((k_u32_t)cid << 16);
    queue->cid++;

    memcpy(&queue->sq[queue->tail], command, sizeof(*command));
    __asm__ volatile("mfence" : : : "memory");

    queue->tail = (k_u16_t)((queue->tail + 1u) % queue->entries);
    doorbell(queue->sq_db, queue->tail);

    if (nvme_completion(queue) != 0) {
        kerror("nvme0: the command 0x%02x did not complete", (unsigned)(command->cdw0 & 0xFFu));
        return -1;
    }

    return 0;
}

/* --- the physical address of a buffer ------------------------------------- */

/*
 * The two pages of a request of at most two pages: PRP1 is the first piece
 * (the page of the buffer and the offset in it) and PRP2 the page after it
 * when the transfer leaves the first page. A buffer that starts in the middle
 * of a page is the case the specification calls the offset of PRP1. False is
 * a buffer this request may not name: a page that does not translate, or one
 * that would need a PRP list (the caller splits by NVME_TRANSFER_MAX).
 */
static bool nvme_prp(const k_u8_t *buf, k_size_t bytes, k_u64_t *prp1, k_u64_t *prp2)
{
    k_size_t inside = (k_size_t)((uintptr_t)buf & (g_page - 1u));
    k_size_t piece = g_page - inside;
    k_u64_t first = port_x86_64_dma_phys(buf);
    k_u64_t second;

    *prp1 = 0;
    *prp2 = 0;

    if (first == 0u || bytes == 0u) {
        return false;
    }

    /* The translation keeps the offset of the page, so the frame may be named
     * with the bytes of the buffer in it; a translator that moved the bytes
     * inside the page would break the piece arithmetic above. */
    if ((first & (g_page - 1u)) != inside) {
        return false;
    }

    if (bytes <= piece) {
        *prp1 = first;
        return true;
    }

    if (bytes > piece + g_page) {
        return false;
    }

    second = port_x86_64_dma_phys(buf + piece);

    if (second == 0u || (second & (g_page - 1u)) != 0u) {
        return false;
    }

    *prp1 = first;
    *prp2 = second;

    return true;
}

/*
 * One command of the NVM set that moves bytes: the address of the first
 * block, the count of the sectors of 512 bytes minus one (the field of the
 * command is 0-based) and the two pages of the buffer. READ (0x02) and
 * WRITE (0x01) name the buffer the same way; the direction is the opcode.
 */
static int nvme_rw_dma(k_u32_t opcode, k_u64_t lba, k_size_t bytes, const k_u8_t *buf)
{
    struct nvme_command command;
    k_u64_t prp1;
    k_u64_t prp2;

    if (!nvme_prp(buf, bytes, &prp1, &prp2)) {
        kwarn("nvme0: a buffer of 0x%x byte(s) cannot be named to the controller", (unsigned)bytes);
        return -1;
    }

    memset(&command, 0, sizeof(command));
    command.cdw0 = opcode;
    command.nsid = g_nsid;
    command.prp1_low = (k_u32_t)prp1;
    command.prp1_high = (k_u32_t)(prp1 >> 32);
    command.prp2_low = (k_u32_t)prp2;
    command.prp2_high = (k_u32_t)(prp2 >> 32);
    command.cdw10 = (k_u32_t)lba;                       /* the low half of the address */
    command.cdw11 = (k_u32_t)(lba >> 32);               /* the high half */
    command.cdw12 = (k_u32_t)(bytes / g_lba_size - 1u); /* 0-based */

    return nvme_submit(&g_io, &command);
}

/* One read of the queue. */
static int nvme_read_dma(k_u64_t lba, k_size_t bytes, const k_u8_t *buf)
{
    return nvme_rw_dma(NVME_IO_READ, lba, bytes, buf);
}

/* One write of the queue: the same bytes and the same two pages, the other
 * direction of the transfer. */
static int nvme_write_dma(k_u64_t lba, k_size_t bytes, const k_u8_t *buf)
{
    return nvme_rw_dma(NVME_IO_WRITE, lba, bytes, buf);
}

/* Flush (0x00): the data of the namespace that the controller holds in its
 * own cache is written to the medium. It carries no buffer and no address:
 * the namespace is the subject and the command is the whole of it. */
static int nvme_flush_dma(void)
{
    struct nvme_command command;

    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_IO_FLUSH;
    command.nsid = g_nsid;

    return nvme_submit(&g_io, &command);
}

/* --- the device of the block layer ---------------------------------------- */

static bool nvme_range_ok(k_u32_t sector, k_size_t count)
{
    return g_sectors != 0u && sector < g_sectors && count <= (k_size_t)g_sectors - sector;
}

/*
 * One read of the block layer: the block of the filesystem is two sectors of
 * the medium, a command moves at most two pages and the rest is another
 * command. The lock of the driver is held for the whole run, because one
 * queue pair is one ring and two tasks share it.
 */
static int nvme_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u64_t lba = sector;
    k_size_t bytes;
    int rc = 0;

    K_UNUSED(ctx);

    if (buf == NULL || !nvme_range_ok(sector, count)) {
        return -1;
    }

    if (k_mutex_lock(&g_nvme_lock) != 0) {
        return -1;
    }

    bytes = count * g_lba_size;

    while (bytes > 0u) {
        k_size_t inside = (k_size_t)((uintptr_t)dst & (g_page - 1u));
        k_size_t chunk = NVME_TRANSFER_MAX - inside;

        if (chunk > bytes) {
            chunk = bytes;
        }

        /* A command moves whole sectors of the medium. */
        chunk &= ~(k_size_t)(g_lba_size - 1u);

        if (chunk == 0u || nvme_read_dma(lba, chunk, dst) != 0) {
            kerror("nvme0: the read of %u byte(s) at LBA %llu failed", (unsigned)chunk,
                   (unsigned long long)lba);
            rc = -1;
            break;
        }

        lba += chunk / g_lba_size;
        dst += chunk;
        bytes -= chunk;
    }

    k_mutex_unlock(&g_nvme_lock);

    return rc;
}

/*
 * One write of the block layer: the mirror of nvme_read with WRITE (0x01).
 * The door of the writes has to be open - a mount that asked for it and the
 * rule that allowed it (nvme_writes) - and the request has to be above the
 * table of the partitions. A medium of another system never reaches this
 * function: the sentence of the rule is the answer of the door.
 */
static int nvme_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    const k_u8_t *source = (const k_u8_t *)buf;
    k_u64_t lba = sector;
    k_size_t bytes;
    int rc = 0;

    K_UNUSED(ctx);

    if (buf == NULL || !nvme_range_ok(sector, count)) {
        return -1;
    }

    if (!g_write_open) {
        if (!g_refused) {
            g_refused = true;
            kwarn("nvme0: the write of %u sector(s) at %u is refused: %s", (unsigned)count,
                  (unsigned)sector,
                  g_refusal != NULL ? g_refusal
                                    : "no mount has asked for the writes of this volume");
        } else {
            kdebug("nvme0: another write refused at %u", (unsigned)sector);
        }

        return -1;
    }

    if (!k_volume_write_above_table(&g_disk_id, sector, count)) {
        if (!g_refused) {
            g_refused = true;
            kwarn("nvme0: the write at %u is refused: the table of the partitions lives there",
                  (unsigned)sector);
        }

        return -1;
    }

    if (k_mutex_lock(&g_nvme_lock) != 0) {
        return -1;
    }

    bytes = count * g_lba_size;

    while (bytes > 0u) {
        k_size_t inside = (k_size_t)((uintptr_t)source & (g_page - 1u));
        k_size_t chunk = NVME_TRANSFER_MAX - inside;

        if (chunk > bytes) {
            chunk = bytes;
        }

        chunk &= ~(k_size_t)(g_lba_size - 1u);

        if (chunk == 0u || nvme_write_dma(lba, chunk, source) != 0) {
            kerror("nvme0: the write of %u byte(s) at LBA %llu failed", (unsigned)chunk,
                   (unsigned long long)lba);
            rc = -1;
            break;
        }

        lba += chunk / g_lba_size;
        source += chunk;
        bytes -= chunk;
    }

    k_mutex_unlock(&g_nvme_lock);

    return rc;
}

/* Bring the writes of the namespace to the medium: the Flush command of the
 * NVM set, the answer to "does the file survive a reset". */
static int nvme_sync(void *ctx)
{
    int rc;

    K_UNUSED(ctx);

    if (!g_write_open) {
        return 0;
    }

    if (k_mutex_lock(&g_nvme_lock) != 0) {
        return -1;
    }

    rc = nvme_flush_dma();

    k_mutex_unlock(&g_nvme_lock);

    if (rc != 0) {
        kerror("nvme0: the namespace does not flush");
    }

    return rc;
}

/*
 * The door of the writes: a mount that asked for them and the rule of
 * ADR 0041. The rule is read again here, and not only at the mount, because
 * the list it reads lives in `/etc/volumes` of the volume itself: the first
 * boot has no root to read it from until the root is mounted, and the driver
 * is asked at the mount. A refusal is KERR_ROFS and a sentence in the log.
 */
static int nvme_writes(void *ctx, bool open)
{
    K_UNUSED(ctx);

    if (!open) {
        g_write_open = false;
        return 0;
    }

    if (g_volume == NULL) {
        return KERR_ROFS;
    }

    if (k_volume_identify(g_volume, &g_id) != 0 ||
        !k_volume_may_write(g_volume, &g_id, &g_refusal)) {
        if (!g_refused) {
            g_refused = true;
            kwarn("nvme0: the writes of %s are refused: %s",
                  g_volume->name != NULL ? g_volume->name : "the disk",
                  g_refusal != NULL ? g_refusal : "the rule of ADR 0041");
        }

        g_read_only = true;
        return KERR_ROFS;
    }

    g_read_only = false;
    g_write_open = true;
    kinfo("nvme0: the writes of %s are open: the volume is named in /etc/volumes",
          g_volume->name != NULL ? g_volume->name : "the disk");

    return 0;
}

static const struct k_blockdev_ops g_nvme_ops = {
    .read = nvme_read,
    .write = nvme_write,
    .erase = NULL, /* a disk keeps its sectors until they are written */
    .sync = nvme_sync,
    .writes = nvme_writes,
};

static struct k_blockdev g_nvme_device = {
    .name = "nvme0",
    .sector_size = 0, /* the formatted LBA size, read by the identify */
    .sectors = 0,     /* the size of the namespace, read by the identify */
    .ops = &g_nvme_ops,
    .ctx = NULL,
    .read_ahead = 0,
};

/* The name the rule gives a medium, for a sentence of the boot. */
static const char *volume_kind_name(const struct k_volume_id *id)
{
    if (id->kind == K_VOLUME_OURS) {
        return "a volume of this system";
    }

    if (id->kind == K_VOLUME_FOREIGN) {
        return "the volume of another system";
    }

    if (id->kind == K_VOLUME_BLANK) {
        return "a medium nothing was written to";
    }

    return "a medium this system does not recognise";
}

/* --- the controller and its queues ---------------------------------------- */

static k_u32_t read_le32(const k_u8_t *bytes)
{
    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

static k_u64_t read_le64(const k_u8_t *bytes)
{
    return (k_u64_t)read_le32(bytes) | ((k_u64_t)read_le32(bytes + 4) << 32);
}

/* The model and the serial number of the controller, as the ASCII the
 * identify data carries: the spaces after the text are not part of it. */
static void text_of(char *out, k_size_t size, const k_u8_t *bytes, k_size_t length)
{
    k_size_t i;

    for (i = 0; i < length && i + 1u < size; i++) {
        out[i] = (char)bytes[i];
    }

    out[i] = '\0';

    while (i > 0u && (out[i - 1u] == ' ' || out[i - 1u] == '\0')) {
        out[--i] = '\0';
    }
}

/*
 * The admin queue and the enabled controller: the sizes, the two physical
 * addresses and CC with the two entry sizes and the page size. The controller
 * comes out of a reset disabled; a firmware that left it enabled is disabled
 * first, because AQA and the two addresses are the host's to program.
 */
static bool nvme_start(void)
{
    k_u64_t cap = reg_read64(NVME_CAP);
    k_u32_t mqes = NVME_CAP_MQES(cap);
    k_u32_t mpsmin = NVME_CAP_MPSMIN(cap);
    k_u32_t cc;

    g_dstrd = NVME_CAP_DSTRD(cap);
    g_page = NVME_PROTOCOL_PAGE;
    g_admin.entries = NVME_ADMIN_ENTRIES;
    g_admin.sq = g_admin_sq;
    g_admin.cq = g_admin_cq;
    g_admin.tail = 0;
    g_admin.head = 0;
    g_admin.phase = NVME_STATUS_PHASE;
    g_admin.cid = 1;
    /* The stride of the doorbells: 4 bytes for every queue until a function
     * says otherwise, and the admin queue is the first pair of them. */
    g_admin.sq_db = 0;
    g_admin.cq_db = 1u * (4u << g_dstrd);

    kinfo("nvme0: CAP 0x%x: %u entries of a queue at most, a stride of %u, CSS 0x%x", (unsigned)cap,
          (unsigned)(mqes + 1u), (unsigned)(4u << g_dstrd), (unsigned)NVME_CAP_CSS(cap));

    if ((NVME_CAP_CSS(cap) & NVME_CSS_NVM) == 0u) {
        kerror("nvme0: the controller has no NVM command set");
        return false;
    }

    if (mqes + 1u < NVME_IO_ENTRIES) {
        g_io.entries = (k_u16_t)(mqes + 1u);
    } else {
        g_io.entries = NVME_IO_ENTRIES;
    }

    /* The page size of the controller is the smallest it takes; the driver
     * names the pages of a buffer in that unit, and this release speaks the
     * page of the x86-64 machine (4 KiB) alone. */
    if (mpsmin != 0u) {
        kerror("nvme0: the controller wants pages of %u bytes, this system names %u",
               (unsigned)(NVME_PROTOCOL_PAGE << mpsmin), (unsigned)NVME_PROTOCOL_PAGE);
        return false;
    }

    g_page = NVME_PROTOCOL_PAGE;

    reg_write32(NVME_CC, 0);

    if (!nvme_wait_ready(false)) {
        kerror("nvme0: the controller does not come out of the enabled state");
        return false;
    }

    reg_write32(NVME_AQA, (NVME_ADMIN_ENTRIES - 1u) * 0x00010001u);
    reg_write64(NVME_ASQ, port_x86_64_kernel_phys(g_admin_sq));
    reg_write64(NVME_ACQ, port_x86_64_kernel_phys(g_admin_cq));

    cc = NVME_CC_EN | ((k_u32_t)NVME_SQ_ENTRY_SHIFT << NVME_CC_IOSQES_SHIFT) |
         ((k_u32_t)NVME_CQ_ENTRY_SHIFT << NVME_CC_IOCQES_SHIFT) | (mpsmin << NVME_CC_MPS_SHIFT);
    reg_write32(NVME_CC, cc);

    if (!nvme_wait_ready(true)) {
        kerror("nvme0: the controller did not become ready");
        return false;
    }

    return true;
}

/* One Identify of the admin queue: the controller (CNS 1, NSID 0) or the
 * namespace (CNS 0). The data is one page, so PRP1 names it whole. */
static bool nvme_identify(k_u32_t nsid, k_u32_t cns)
{
    struct nvme_command command;
    k_u64_t buffer = port_x86_64_kernel_phys(g_identify);

    if ((buffer & (g_page - 1u)) != 0u) {
        kerror("nvme0: the identify buffer is not page aligned");
        return false;
    }

    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_IDENTIFY;
    command.nsid = nsid;
    command.prp1_low = (k_u32_t)buffer;
    command.prp1_high = (k_u32_t)(buffer >> 32);
    command.cdw10 = cns;

    return nvme_submit(&g_admin, &command) == 0;
}

/*
 * The one I/O queue pair of the driver: the completion queue first (the
 * submission queue names it), both with the flag of a physically contiguous
 * queue, and the completion queue without an interrupt vector because the
 * driver polls. The two queues are the pair 1 of the controller, and the
 * doorbells follow the admin pair in the row.
 */
static bool nvme_io_queue(void)
{
    struct nvme_command command;
    k_u16_t entries = g_io.entries;
    k_u32_t stride = 4u << g_dstrd;

    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_CREATE_CQ;
    command.prp1_low = (k_u32_t)port_x86_64_kernel_phys(g_io_cq);
    command.prp1_high = (k_u32_t)(port_x86_64_kernel_phys(g_io_cq) >> 32);
    command.cdw10 = 1u | ((k_u32_t)(entries - 1u) << 16); /* QID 1, QSIZE */
    command.cdw11 = NVME_QUEUE_PC;                        /* polled: no IEN */

    if (nvme_submit(&g_admin, &command) != 0) {
        kerror("nvme0: the completion queue of the I/O pair is refused");
        return false;
    }

    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_CREATE_SQ;
    command.prp1_low = (k_u32_t)port_x86_64_kernel_phys(g_io_sq);
    command.prp1_high = (k_u32_t)(port_x86_64_kernel_phys(g_io_sq) >> 32);
    command.cdw10 = 1u | ((k_u32_t)(entries - 1u) << 16);
    command.cdw11 = NVME_QUEUE_PC | (1u << 16); /* PC | the completion queue 1 */

    if (nvme_submit(&g_admin, &command) != 0) {
        kerror("nvme0: the submission queue of the I/O pair is refused");
        return false;
    }

    g_io.sq = g_io_sq;
    g_io.cq = g_io_cq;
    g_io.tail = 0;
    g_io.head = 0;
    g_io.phase = NVME_STATUS_PHASE;
    g_io.cid = 1;
    g_io.sq_db = 2u * 1u * stride;        /* the tail of the queue 1 */
    g_io.cq_db = (2u * 1u + 1u) * stride; /* the head of the queue 1 */

    return true;
}

/* --- the boot ------------------------------------------------------------- */

/*
 * The window of the registers: BAR0 is a 64-bit memory window (the NVMe
 * function of a laptop always is), and it sits above the identity map of the
 * first gigabyte, so it is mapped uncached into the top 2 GiB as the screen,
 * the APIC and the controller of the SATA disk are (ADR 0023, ADR 0041).
 */
static volatile k_u8_t *nvme_map(const struct port_x86_64_pci_device *card)
{
    k_u32_t bar = port_x86_64_pci_read32(card, PCI_BAR0);
    k_u64_t base;
    k_u64_t offset;

    if ((bar & PCI_BAR_IO) != 0u) {
        kwarn("nvme0: BAR0 is a range of ports, not the window of a controller");
        return NULL;
    }

    base = bar & ~(k_u64_t)PCI_BAR_MEMORY_MASK;

    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        base |= (k_u64_t)port_x86_64_pci_read32(card, PCI_BAR1) << 32;
    }

    if (base == 0u) {
        return NULL;
    }

    offset = base & 0x1FFFFFull;

    if (port_x86_64_paging_map_mmio(PORT_X86_64_NVME_VIRT, base - offset,
                                    (k_size_t)(offset + 0x4000u)) == 0u) {
        kwarn("nvme0: the registers at 0x%x cannot be mapped", (unsigned)base);
        return NULL;
    }

    return (volatile k_u8_t *)(uintptr_t)(PORT_X86_64_NVME_VIRT + offset);
}

/*
 * The controller, its namespace and the disk. A machine without the function,
 * without a namespace, or with a namespace of another block size (the release
 * speaks 512 bytes) registers nothing and the boot continues on the medium it
 * had.
 */
void port_x86_64_nvme_init(void)
{
    struct port_x86_64_pci_device card;
    const struct k_blockdev *disk;
    const struct k_blockdev *volume;
    const char *first;
    k_u64_t namespace_size;
    k_u32_t lbaf;
    k_u32_t lbads;
    k_u32_t namespaces;
    k_u32_t flbas;
    char model[41];
    char serial[21];
    int partitions;

    if (!port_x86_64_pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, 0x00u, &card)) {
        return;
    }

    g_regs = nvme_map(&card);

    if (g_regs == NULL) {
        return;
    }

    port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    if (!nvme_start()) {
        kerror("nvme0: the controller at %u:%u.%u does not start", (unsigned)card.bus,
               (unsigned)card.slot, (unsigned)card.function);
        return;
    }

    /* Identify Controller: the model, the serial and the count of the
     * namespaces. */
    memset(g_identify, 0, sizeof(g_identify));

    if (!nvme_identify(0u, NVME_IDENTIFY_CONTROLLER)) {
        kerror("nvme0: the controller did not identify itself");
        return;
    }

    text_of(model, sizeof(model), g_identify + 24u, 40u);
    text_of(serial, sizeof(serial), g_identify + 4u, 20u);
    namespaces = read_le32(g_identify + 516u);

    kinfo("nvme0: %s %s, %u namespace(s), NVMe %u.%u.%u", model, serial, (unsigned)namespaces,
          (unsigned)(reg_read32(NVME_VS) >> 16), (unsigned)((reg_read32(NVME_VS) >> 8) & 0xFFu),
          (unsigned)(reg_read32(NVME_VS) & 0xFFu));

    if (namespaces == 0u) {
        kinfo("nvme0: the controller names no namespace, nothing is registered");
        return;
    }

    /* Identify Namespace 1: how many blocks of which size it formats. */
    memset(g_identify, 0, sizeof(g_identify));

    if (!nvme_identify(g_nsid, NVME_IDENTIFY_NAMESPACE)) {
        kerror("nvme0: the namespace %u did not identify itself", (unsigned)g_nsid);
        return;
    }

    namespace_size = read_le64(g_identify);
    flbas = g_identify[26] & 0x0Fu;
    lbaf = read_le32(g_identify + 128u + flbas * 4u);
    lbads = (lbaf >> 16) & 0xFFu;

    if (lbads < 9u || lbads > 12u) {
        kerror("nvme0: the namespace formats %u-byte blocks, this driver supports 512 and 4096",
               (unsigned)(1u << lbads));
        return;
    }

    g_lba_size = 1u << lbads;

    if (namespace_size == 0u) {
        kerror("nvme0: the namespace has no sectors");
        return;
    }

    if (namespace_size > 0xFFFFFFFFull) {
        namespace_size = 0xFFFFFFFFull;
    }

    if (!nvme_io_queue()) {
        return;
    }

    g_sectors = (k_u32_t)namespace_size;
    g_nvme_device.sector_size = g_lba_size;
    g_nvme_device.sectors = g_sectors;
    g_nvme_device.read_ahead = NVME_READ_AHEAD_BYTES / g_lba_size;
    if (g_nvme_device.read_ahead == 0u) {
        g_nvme_device.read_ahead = 1u;
    }

    kinfo("nvme0: %u MB, %u-byte sectors, %u entries in the pair of the I/O queues",
          (unsigned)(((k_u64_t)g_sectors * g_lba_size) / (1024u * 1024u)), (unsigned)g_lba_size,
          (unsigned)g_io.entries);

    /*
     * The cache of the block layer, write-through: the disk is read-only and
     * a write has to come back as the refusal of the driver, not as a sector
     * that sat in the cache until a sync nobody asked for (ADR 0041).
     */
    {
        k_size_t cache_sectors = NVME_CACHE_BYTES / g_lba_size;
        if (cache_sectors == 0u) {
            cache_sectors = 1u;
        }
        if (k_blockdev_register_cached_sized(&g_nvme_device, false, cache_sectors) != 0) {
            kerror("nvme0: cannot register the disk");
            return;
        }
    }

    /* The view the table hands out is the one with the cache above it, and
     * the rule reads the same bytes a mount would read. */
    disk = k_blockdev_find(g_nvme_device.name);

    if (disk == NULL) {
        disk = &g_nvme_device;
    }

    /*
     * The whole disk first: its table says where the writes may not go. The
     * volume is the partition the boot will mount, so the superblock the rule
     * reads is the one the filesystem is about to read.
     */
    if (k_volume_identify(disk, &g_disk_id) != 0) {
        kerror("nvme0: the head of the disk cannot be read, it is not used");
        return;
    }

    partitions = partition_scan(disk);

    if (partitions < 0 && partitions != KERR_NOENT) {
        kerror("nvme0: the partition table is not usable (%d)", partitions);
    }

    first = partitions > 0 ? partition_name(disk, 0) : NULL;
    volume = first != NULL ? k_blockdev_find(first) : disk;

    if (volume == NULL) {
        volume = disk;
    }

    if (k_volume_identify(volume, &g_id) != 0) {
        kerror("nvme0: the head of %s cannot be read, it is not used",
               volume->name != NULL ? volume->name : "the disk");
        return;
    }

    /*
     * The namespace is registered read-only, always: the rule of ADR 0041
     * is a decision of a mount, and no mount has asked yet. The list it
     * reads - `/etc/volumes` - lives inside the volume itself, so the rule
     * cannot answer before the root is mounted; nvme_writes() asks it when a
     * mount does.
     */
    g_volume = volume;
    g_read_only = true;
    g_write_open = false;
    g_refusal = NULL;

    if (g_id.kind != K_VOLUME_OURS) {
        (void)k_volume_may_write(volume, &g_id, &g_refusal);
    }

    kinfo("nvme0: %s: %s (%s), read-only until a mount asks for the writes",
          volume->name != NULL ? volume->name : "the disk", volume_kind_name(&g_id),
          g_id.format != NULL ? g_id.format : "no superblock");

    if (g_refusal != NULL) {
        kinfo("nvme0: the disk is read-only: %s", g_refusal);
    }

    if (partitions == KERR_NOENT) {
        kinfo("nvme0: no partition table, the whole disk would be the volume");
    } else if (first == NULL) {
        kinfo("nvme0: the table names no partition this kernel reads");
    } else {
        g_root = first;
        kinfo("x86-64: the root volume is %s (%d partition(s))", first, partitions);
    }

    /* The sentence of the first boot with a new controller: what was found,
     * and that nothing was written. */
    if (g_read_only) {
        kinfo("nvme: 1 disk(s) on the controller, read-only, nothing written");
    } else {
        kinfo("nvme: 1 disk(s) on the controller, writable");
    }
}

const char *port_x86_64_nvme_root(void)
{
    return g_root;
}
