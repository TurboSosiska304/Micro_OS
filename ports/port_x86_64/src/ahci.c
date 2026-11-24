/*
 * Micro OS — the x86-64 port: the disk of a chipset, on the AHCI controller.
 *
 * The two disks of the port before this one were the channel of 1984
 * (src/ata.c) and a card of virtio (src/virtio_blk.c). A machine of these
 * years has neither: the SATA controller is a function of the chipset with a
 * window of registers in the memory (a BAR above the identity map), one
 * command list per port, and a table of physical addresses for the bytes.
 *
 * This file is the third medium of the port, and it is a reader: the disk a
 * chipset brings was not formatted by this system, and ADR 0041 says such a
 * disk is read-only until the volume says otherwise. The rule is
 * kernel/volume.c - the header block of MicroFS, the list of /etc/volumes and
 * the first sector of the partition - and this driver is its first client:
 * the boot prints what the controller found, registers the volume, mounts it
 * and writes not one byte.
 *
 * The protocol of the controller, in the order a command needs it:
 *
 *   the ABAR (BAR5) is a 64-bit window of registers, mapped uncached into the
 *   top 2 GiB because it sits above the identity map of the first gigabyte;
 *   CAP, GHC, IS and PI come first, and GHC.AE lets the controller speak
 *   AHCI instead of the legacy emulation;
 *
 *   every port has a command list of 32 headers (PxCLB/PxCLBU), an area for
 *   the FIS the device sends (PxFB/PxFBU) and the registers that start it
 *   (PxCMD.ST and PxCMD.FRE). One command is a header, a table with the
 *   FIS_REG_H2D of the command and a PRDT of physical addresses;
 *
 *   the command is issued by writing the port's bit in PxCI, and the port
 *   answers by clearing it: this driver polls that bit with a budget of
 *   time, the pattern of the PIO path of src/ata.c, because one disk of a
 *   boot is a read and the interrupt of every port is a line of the I/O APIC
 *   that this step does not open yet. The line is the next step of the lane;
 *
 *   IDENTIFY DEVICE (0xEC) says how many sectors of 512 bytes the drive has,
 *   READ DMA EXT (0x25) moves them, and WRITE DMA EXT (0x35) would - it is
 *   the command the rule above refuses before the controller is programmed.
 *
 * The address of a buffer is translated per page, as ADR 0032 says for every
 * bus master of the port: the filesystem may hand the driver the buffer of a
 * program, and the page tables of the program are the only place that knows
 * where its frame is. There is no PIO path behind this driver to fall back
 * on, so a buffer that does not translate ends the request with a failure of
 * the whole read.
 *
 * The device is optional: a machine with no AHCI function, and every machine
 * of the releases before this one, registers no disk here and boots the disk
 * it always booted.
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

#include "port_x86_64_ahci.h"
#include "port_x86_64_pci.h"

/* --- the function on the PCI bus ------------------------------------------ */

#define PCI_CLASS_STORAGE 0x01u
#define PCI_SUBCLASS_SATA 0x06u
/* The programming interface of a controller in the AHCI mode: bit 0 (bit 1
 * is the same hardware in the IDE mode, which this driver does not speak,
 * and bit 7 is the bus master every one of them has). */
#define PCI_PROG_IF_AHCI 0x01u

#define PCI_BAR5 0x24u
#define PCI_BAR6 0x28u /* the upper half of a 64-bit BAR5 */
#define PCI_BAR_TYPE_MASK 0x06u
#define PCI_BAR_TYPE_64 0x04u
#define PCI_BAR_MEMORY_MASK 0x0Fu

/* --- the registers of the controller -------------------------------------- */

#define AHCI_CAP 0x00u /* the capabilities: 64-bit addressing, ports, ... */
#define AHCI_GHC 0x04u /* the global control: AE lets the controller speak */
#define AHCI_IS 0x08u  /* the ports with an interrupt to report */
#define AHCI_PI 0x0Cu  /* the ports that are implemented */
#define AHCI_VS 0x10u  /* the version of the specification */

#define AHCI_GHC_AE 0x80000000u
#define AHCI_CAP_S64A 0x80000000u

/* The window of one port, and the registers inside it. */
#define AHCI_PORT_BASE 0x100u
#define AHCI_PORT_STRIDE 0x80u
#define AHCI_PORTS 32u

#define AHCI_P_CLB 0x00u /* the command list, its physical address */
#define AHCI_P_CLBU 0x04u
#define AHCI_P_FB 0x08u /* the FIS the device sends */
#define AHCI_P_FBU 0x0Cu
#define AHCI_P_IS 0x10u /* the interrupts of the port */
#define AHCI_P_IE 0x14u
#define AHCI_P_CMD 0x18u  /* start, FIS receive, power, ... */
#define AHCI_P_TFD 0x20u  /* the status of the task file */
#define AHCI_P_SIG 0x24u  /* what kind of device is on the port */
#define AHCI_P_SSTS 0x28u /* the state of the link */
#define AHCI_P_SCTL 0x2Cu
#define AHCI_P_SERR 0x30u
#define AHCI_P_SACT 0x34u
#define AHCI_P_CI 0x38u /* the commands in flight, one bit a slot */

#define AHCI_P_CMD_ST 0x00000001u  /* start the command engine */
#define AHCI_P_CMD_SUD 0x00000002u /* spin the device up */
#define AHCI_P_CMD_POD 0x00000004u /* the port is powered */
#define AHCI_P_CMD_CLO 0x00000008u /* a command is being cleared */
#define AHCI_P_CMD_FRE 0x00000010u /* receive the FIS of the device */
#define AHCI_P_CMD_FR 0x00004000u  /* the FIS receive engine runs */
#define AHCI_P_CMD_CR 0x00008000u  /* the command engine runs */

#define AHCI_P_IS_TFES 0x40000000u /* a task file error: the command failed */
#define AHCI_P_IS_INFES 0x20000000u
#define AHCI_P_IS_IF 0x08000000u /* the FIS of the device cannot be read */

#define AHCI_P_TFD_ERR 0x00000001u
#define AHCI_P_TFD_DRQ 0x00000008u
#define AHCI_P_TFD_BSY 0x00000080u

/* PxSSTS: the four bits of the device detection say "a device is there and
 * the link is up" with the number 3. */
#define AHCI_P_SSTS_DET_MASK 0x0Fu
#define AHCI_P_SSTS_DET_READY 0x03u

/* PxSIG: the signature of a drive of the SATA channel, which is an ATA disk
 * and not a CD-ROM, a port multiplier or an enclosure. */
#define AHCI_P_SIG_ATA 0x00000101u

/* --- the commands of the ATA set ------------------------------------------ */

#define AHCI_CMD_READ_DMA_EXT 0x25u
#define AHCI_CMD_WRITE_DMA_EXT 0x35u
#define AHCI_CMD_IDENTIFY 0xECu

#define AHCI_FIS_REG_H2D 0x27u
#define AHCI_FIS_COMMAND 0x80u /* bit 7 of the second byte: "this is a command" */

#define AHCI_MEDIUM_SECTOR 512u
#define AHCI_SECTORS_PER_BLOCK (MFS_BLOCK_SIZE / AHCI_MEDIUM_SECTOR)
#define AHCI_PAGE 4096u

_Static_assert(MFS_BLOCK_SIZE % AHCI_MEDIUM_SECTOR == 0,
               "a block of the filesystem is not a whole number of disk sectors");

/* The command list of a port is 32 headers of 32 bytes, and the entry the
 * driver uses is always the first: one command of the port is in flight at a
 * time, and the slot is free the moment the port clears its bit of PxCI. */
#define AHCI_SLOTS 32u
#define AHCI_SLOT 0u

/* The table of one command: the FIS (64 bytes), the ATAPI command (16), the
 * reserved 48 and then the PRDT. A table of 4 KiB holds 244 entries, and the
 * driver uses 128 of them: 128 pages of 512 bytes are 64 KiB of a command,
 * which is the run a read ahead of a file asks of the cache. */
#define AHCI_CFIS 0u
#define AHCI_ACMD 64u
#define AHCI_PRDT 128u
#define AHCI_PRDT_ENTRIES 128u
#define AHCI_PRDT_MAX 0x3FFFFu                       /* the count of a PRDT entry is 18 bits */
#define AHCI_TRANSFER_MAX (AHCI_PRDT_ENTRIES * 512u) /* 64 KiB by command */

#define AHCI_PRDT_DBC_INTERRUPT 0x80000000u

/* A device that does not answer must not hang the boot: every wait gives up
 * after this much time, and a machine whose processor the port could not
 * calibrate gives up after this many polls instead. */
#define AHCI_TIMEOUT_MS 5000u
#define AHCI_FALLBACK_POLLS 50000000u

/* A command that fails is brought back once, with the port started again:
 * the first failure of a boot is often a stale state of the engine. */
#define AHCI_ATTEMPTS 2u

/*
 * The cache of the block layer above this disk, and the run a read ahead of
 * a file asks of it: the same numbers as the two other disks of the port
 * (ADR 0032), because the medium is the same kind of medium and the commands
 * of the controller move the same 64 KiB.
 *
 * The cache is write-through on purpose: the disk is read-only, and a write
 * that the cache held back would be answered "yes" to the filesystem and
 * refused by the driver a moment later. A write-through cache asks the
 * driver for every sector, so the refusal is the answer of the call.
 */
#define AHCI_CACHE_SECTORS 512u
#define AHCI_READ_AHEAD 128u

/* --- the structures of a command ------------------------------------------ */

/*
 * One entry of the command list: the length of the FIS in words, the flags
 * of the command, the length of the PRDT in entries, and the physical
 * address of the table. The fields the driver does not use are zero.
 */
struct ahci_header {
    k_u32_t flags; /* CFL | A | W | PRDTL << 16 */
    k_u32_t transferred;
    k_u32_t table;
    k_u32_t table_hi;
    k_u32_t reserved[4];
};

#define AHCI_HEADER_CFL(words) ((k_u32_t)(words) & 0x1Fu)
#define AHCI_HEADER_W (1u << 6) /* the data goes from the memory to the device */
#define AHCI_HEADER_PRDTL(entries) ((k_u32_t)(entries) << 16)

_Static_assert(sizeof(struct ahci_header) == 32u, "the header of a command changed");

/*
 * The FIS that carries a command (SATA 3.2, the Register Host-to-Device FIS):
 * the type, the byte of the flags (the C bit and the port multiplier), the
 * command, the feature and the address of six bytes, the device register,
 * the count of the sectors, and the control. The reply of the device is
 * another FIS, in the area the port points at.
 */
struct ahci_fis {
    k_u8_t type;         /* 0: 0x27 */
    k_u8_t flags;        /* 1: C in bit 7, the port multiplier in the low four */
    k_u8_t command;      /* 2 */
    k_u8_t feature_low;  /* 3 */
    k_u8_t lba0;         /* 4: LBA 7:0 */
    k_u8_t lba1;         /* 5: LBA 15:8 */
    k_u8_t lba2;         /* 6: LBA 23:16 */
    k_u8_t device;       /* 7 */
    k_u8_t feature_high; /* 8 */
    k_u8_t lba3;         /* 9: LBA 31:24 */
    k_u8_t lba4;         /* 10: LBA 39:32 */
    k_u8_t lba5;         /* 11: LBA 47:40 */
    k_u8_t count_low;    /* 12 */
    k_u8_t count_high;   /* 13 */
    k_u8_t icc;          /* 14 */
    k_u8_t control;      /* 15 */
    k_u8_t reserved[48];
};

/* One entry of the PRDT: the physical address of a piece of the buffer, its
 * length minus one, and the bit that ends the transfer with an interrupt. */
struct ahci_prdt {
    k_u32_t address;
    k_u32_t address_hi;
    k_u32_t reserved;
    k_u32_t count;
};

_Static_assert(sizeof(struct ahci_prdt) == 16u, "the entry of a PRDT changed");

/* --- the state of the driver ---------------------------------------------- */

/*
 * The memory of the controller: the command list (1 KiB), the table of the
 * one command (4 KiB) and the area the FIS of the device lands in (1 KiB).
 * All three are physical addresses the controller reads; they live in the
 * image of the kernel, which the port translates with the one function that
 * knows where the image is (port_x86_64_kernel_phys).
 */
static struct ahci_header g_cmdlist[AHCI_SLOTS] __attribute__((aligned(1024)));
static k_u8_t g_cmdtable[4096] __attribute__((aligned(4096)));
static k_u8_t g_fis[1024] __attribute__((aligned(1024)));

static volatile k_u32_t *g_hba; /* the window of the registers */
static struct port_x86_64_pci_device g_card;
static k_u32_t g_port;               /* the number of the port the disk is on */
static k_u32_t g_ports;              /* the ports the controller implements */
static k_u32_t g_medium_sectors;     /* the sectors of 512 bytes of the drive */
static k_u32_t g_sectors;            /* the blocks of the filesystem on it */
static const char *g_root;           /* the partition the boot mounts, if any */
static bool g_read_only = true;      /* the rule of ADR 0041, decided once */
static const char *g_refusal;        /* the sentence of the rule, for the log */
static bool g_refused;               /* a write was refused and is in the log */
static struct k_volume_id g_id;      /* what the rule saw on the volume */
static struct k_volume_id g_disk_id; /* the whole disk: the table of the writes */
static k_mutex_t g_ahci_lock = K_MUTEX_INITIALIZER;

/* --- one register of the controller --------------------------------------- */

static k_u32_t reg_read(k_u32_t offset)
{
    return g_hba[offset / 4u];
}

static void reg_write(k_u32_t offset, k_u32_t value)
{
    g_hba[offset / 4u] = value;
}

static k_u32_t port_offset(k_u32_t port, k_u32_t register_offset)
{
    return AHCI_PORT_BASE + port * AHCI_PORT_STRIDE + register_offset;
}

static k_u32_t port_read(k_u32_t port, k_u32_t register_offset)
{
    return reg_read(port_offset(port, register_offset));
}

static void port_write(k_u32_t port, k_u32_t register_offset, k_u32_t value)
{
    reg_write(port_offset(port, register_offset), value);
}

/* The budget of a wait, in the ticks of the time stamp counter; zero when the
 * port could not measure the processor. */
static k_u64_t ahci_timeout_ticks(void)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info->cpu_hz == 0u) {
        return 0;
    }

    return info->cpu_hz / 1000u * AHCI_TIMEOUT_MS;
}

/* Wait until `mask` of the register clears (or sets). A register that never
 * answers is the end of the wait, not a hang of the machine. */
static bool ahci_wait(k_u32_t port, k_u32_t register_offset, k_u32_t mask, bool set)
{
    k_u64_t budget = ahci_timeout_ticks();
    k_u64_t start = port_rdtsc();
    k_u32_t polls = 0;

    for (;;) {
        k_u32_t value = port_read(port, register_offset);

        if (((value & mask) != 0u) == set) {
            return true;
        }

        if (budget != 0u) {
            if (port_rdtsc() - start > budget) {
                return false;
            }
        } else if (++polls >= AHCI_FALLBACK_POLLS) {
            return false;
        }

        __asm__ volatile("pause");
    }
}

/*
 * The physical address behind a byte the transfer names: the shared
 * translation of the port (ADR 0032 §1, port_x86_64_dma_phys). Zero is "this
 * buffer is not one the controller may be given".
 */
static k_u64_t ahci_phys(k_u64_t address)
{
    return port_x86_64_dma_phys((const void *)(uintptr_t)address);
}

/*
 * Cut a buffer into the pieces of a PRDT: one entry per page, because two
 * virtual pages are not two adjacent frames. A piece never crosses a page
 * and the count of an entry is a byte count minus one, so the address must
 * be even - a buffer at an odd address is refused, as the standard says.
 *
 * Returns the number of entries, zero when the buffer cannot be described.
 */
static k_u32_t ahci_prdt_fill(const k_u8_t *buf, k_size_t bytes)
{
    struct ahci_prdt *prdt = (struct ahci_prdt *)(void *)(g_cmdtable + AHCI_PRDT);
    k_size_t done = 0;
    k_u32_t entries = 0;

    while (done < bytes && entries < AHCI_PRDT_ENTRIES) {
        k_u64_t address = (k_u64_t)(uintptr_t)(buf + done);
        k_size_t left = bytes - done;
        k_size_t inside = (k_size_t)(address & (AHCI_PAGE - 1u));
        k_size_t piece = AHCI_PAGE - inside;
        k_u64_t physical;

        /* The count of a descriptor is a count of words: a buffer at an odd
         * address is a buffer the controller is not given. */
        if ((address & 1u) != 0u) {
            return 0;
        }

        if (piece > left) {
            piece = left;
        }

        physical = ahci_phys(address);

        if (physical == 0u || physical + piece > 0x100000000ull) {
            return 0;
        }

        prdt[entries].address = (k_u32_t)physical;
        prdt[entries].address_hi = (k_u32_t)(physical >> 32);
        prdt[entries].reserved = 0;
        prdt[entries].count = (k_u32_t)(piece - 1u);

        entries++;
        done += piece;
    }

    if (done < bytes) {
        return 0;
    }

    /* The last entry ends the transfer with an interrupt; the driver polls,
     * so the line is masked by PxIE and the bit costs nothing, but the
     * controller of a person's chipset waits for it before it clears PxCI. */
    prdt[entries - 1u].count |= AHCI_PRDT_DBC_INTERRUPT;

    return entries;
}

/*
 * Program one command of the port: the FIS, the header, the table and the
 * bit of PxCI. Returns 0 when the controller took it and reported no error.
 */
static int ahci_command(k_u32_t command, bool write, k_u64_t lba, k_u32_t sectors,
                        const k_u8_t *buf, k_size_t bytes)
{
    struct ahci_fis *fis = (struct ahci_fis *)(void *)(g_cmdtable + AHCI_CFIS);
    struct ahci_header *header = &g_cmdlist[AHCI_SLOT];
    k_u64_t table = port_x86_64_kernel_phys(g_cmdtable);
    k_u32_t entries;
    k_u32_t status;

    if (sectors == 0u || bytes == 0u) {
        return -1;
    }

    entries = ahci_prdt_fill(buf, bytes);

    if (entries == 0u) {
        kwarn("ahci0: a buffer of 0x%x byte(s) cannot be named to the controller", (unsigned)bytes);
        return -1;
    }

    memset(fis, 0, sizeof(*fis));
    fis->type = AHCI_FIS_REG_H2D;
    fis->flags = AHCI_FIS_COMMAND;
    fis->command = (k_u8_t)command;
    fis->device = 0x40u; /* the LBA mode of the drive register */
    fis->lba0 = (k_u8_t)(lba & 0xFFu);
    fis->lba1 = (k_u8_t)((lba >> 8) & 0xFFu);
    fis->lba2 = (k_u8_t)((lba >> 16) & 0xFFu);
    fis->lba3 = (k_u8_t)((lba >> 24) & 0xFFu);
    fis->lba4 = (k_u8_t)((lba >> 32) & 0xFFu);
    fis->lba5 = (k_u8_t)((lba >> 40) & 0xFFu);
    fis->count_low = (k_u8_t)(sectors & 0xFFu);
    fis->count_high = (k_u8_t)((sectors >> 8) & 0xFFu);

    memset(header, 0, sizeof(*header));
    header->flags = AHCI_HEADER_CFL(5u) | AHCI_HEADER_PRDTL(entries) | (write ? AHCI_HEADER_W : 0u);
    header->table = (k_u32_t)table;
    header->table_hi = (k_u32_t)(table >> 32);

    /* The FIS area has to be empty before a command: a stale entry is a port
     * that answers the command of somebody else. */
    port_write(g_port, AHCI_P_IS, port_read(g_port, AHCI_P_IS));
    port_write(g_port, AHCI_P_CI, 1u << AHCI_SLOT);

    if (!ahci_wait(g_port, AHCI_P_CI, 1u << AHCI_SLOT, false)) {
        kwarn("ahci0: the command at %u did not finish in %u ms", (unsigned)lba,
              (unsigned)AHCI_TIMEOUT_MS);
        return -1;
    }

    status = port_read(g_port, AHCI_P_IS);
    port_write(g_port, AHCI_P_IS, status);

    if ((status & (AHCI_P_IS_TFES | AHCI_P_IS_INFES | AHCI_P_IS_IF)) != 0u ||
        (port_read(g_port, AHCI_P_TFD) & (AHCI_P_TFD_ERR | AHCI_P_TFD_BSY)) != 0u) {
        kdebug("ahci0: the command at %u failed (IS 0x%x, TFD 0x%x)", (unsigned)lba,
               (unsigned)status, (unsigned)port_read(g_port, AHCI_P_TFD));
        return -1;
    }

    return 0;
}

/* --- the port and its engine ---------------------------------------------- */

/*
 * Stop the engine of the port, program the two areas and start it again: the
 * order of the specification, because a command list that moves under a
 * running engine is a command the controller reads from a half written list.
 */
static bool ahci_port_start(void)
{
    k_u32_t cmd = port_read(g_port, AHCI_P_CMD);
    k_u64_t cmdlist = port_x86_64_kernel_phys(g_cmdlist);
    k_u64_t fis = port_x86_64_kernel_phys(g_fis);

    cmd &= ~(AHCI_P_CMD_ST | AHCI_P_CMD_FRE);
    port_write(g_port, AHCI_P_CMD, cmd);

    if (!ahci_wait(g_port, AHCI_P_CMD, AHCI_P_CMD_CR, false) ||
        !ahci_wait(g_port, AHCI_P_CMD, AHCI_P_CMD_FR, false)) {
        kerror("ahci0: the engine of the port does not stop");
        return false;
    }

    /* Power the port and spin the device up: a disk of a laptop is asleep
     * until somebody says the word. */
    cmd |= AHCI_P_CMD_POD | AHCI_P_CMD_SUD;
    port_write(g_port, AHCI_P_CMD, cmd);

    port_write(g_port, AHCI_P_CLB, (k_u32_t)cmdlist);
    port_write(g_port, AHCI_P_CLBU, (k_u32_t)(cmdlist >> 32));
    port_write(g_port, AHCI_P_FB, (k_u32_t)fis);
    port_write(g_port, AHCI_P_FBU, (k_u32_t)(fis >> 32));

    port_write(g_port, AHCI_P_IS, port_read(g_port, AHCI_P_IS));
    port_write(g_port, AHCI_P_SERR, port_read(g_port, AHCI_P_SERR));

    cmd = port_read(g_port, AHCI_P_CMD);
    port_write(g_port, AHCI_P_CMD, cmd | AHCI_P_CMD_FRE);

    if (!ahci_wait(g_port, AHCI_P_CMD, AHCI_P_CMD_FR, true)) {
        kerror("ahci0: the port does not receive the FIS of the device");
        return false;
    }

    port_write(g_port, AHCI_P_CMD, port_read(g_port, AHCI_P_CMD) | AHCI_P_CMD_ST);

    if (!ahci_wait(g_port, AHCI_P_CMD, AHCI_P_CMD_CR, true)) {
        kerror("ahci0: the command engine of the port does not start");
        return false;
    }

    return true;
}

/* Ask the drive who it is: the identify data of 256 words, with the count of
 * the sectors of 512 bytes in words 60 and 61 (or 100 to 103 of a drive that
 * speaks LBA48 and is larger than the 28 bits of the old command). */
static bool ahci_identify(k_u32_t *medium_sectors)
{
    static k_u16_t identity[256] __attribute__((aligned(8)));
    k_u64_t capacity;
    int rc;

    rc = ahci_command(AHCI_CMD_IDENTIFY, false, 0u, 1u, (const k_u8_t *)identity, sizeof(identity));

    if (rc != 0) {
        return false;
    }

    capacity = (k_u64_t)identity[60] | ((k_u64_t)identity[61] << 16);

    if (capacity == 0u) {
        return false;
    }

    if (capacity > 0xFFFFFFFFull) {
        capacity = 0xFFFFFFFFull;
    }

    *medium_sectors = (k_u32_t)capacity;

    return true;
}

/* --- the device of the block layer ---------------------------------------- */

static bool ahci_range_ok(k_u32_t sector, k_size_t count)
{
    return g_sectors != 0u && sector < g_sectors && count <= (k_size_t)g_sectors - sector;
}

/*
 * One read of the block layer: the sectors of the filesystem are two of the
 * medium, a command moves at most 64 KiB, and the rest is another command.
 * The lock of the driver is held for the whole run, because the table of the
 * command is one and a second task may ask for its own sector between two
 * commands of this one.
 */
static int ahci_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u64_t lba;
    k_size_t bytes;
    int rc = 0;

    K_UNUSED(ctx);

    if (buf == NULL || !ahci_range_ok(sector, count)) {
        return -1;
    }

    if (k_mutex_lock(&g_ahci_lock) != 0) {
        return -1;
    }

    lba = (k_u64_t)sector * AHCI_SECTORS_PER_BLOCK;
    bytes = count * MFS_BLOCK_SIZE;

    while (bytes > 0u) {
        k_size_t chunk = bytes > AHCI_TRANSFER_MAX ? AHCI_TRANSFER_MAX : bytes;
        k_u32_t attempt;

        for (attempt = 0; attempt < AHCI_ATTEMPTS; attempt++) {
            rc = ahci_command(AHCI_CMD_READ_DMA_EXT, false, lba,
                              (k_u32_t)(chunk / AHCI_MEDIUM_SECTOR), dst, chunk);

            if (rc == 0) {
                break;
            }

            /* A port whose engine lost its state is started again; the
             * areas are the same and the command is repeated once. */
            if (!ahci_port_start()) {
                break;
            }
        }

        if (rc != 0) {
            kerror("ahci0: the read of %u sector(s) at %u failed", (unsigned)chunk, (unsigned)lba);
            break;
        }

        lba += chunk / AHCI_MEDIUM_SECTOR;
        dst += chunk;
        bytes -= chunk;
    }

    k_mutex_unlock(&g_ahci_lock);

    return rc;
}

/*
 * The write the rule of ADR 0041 refuses. There are two sentences: the disk
 * was not made by this system (the superblock and the list of /etc/volumes),
 * and the request names the table of the partitions. The first is decided
 * once at the boot; the second is asked per request, because a disk that a
 * later step makes writable still may not be written below its first
 * partition.
 */
static int ahci_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(buf);

    if (!ahci_range_ok(sector, count)) {
        return -1;
    }

    if (g_read_only) {
        if (!g_refused) {
            g_refused = true;
            kwarn("ahci0: the write of %u sector(s) at %u is refused: %s", (unsigned)count,
                  (unsigned)sector, g_refusal != NULL ? g_refusal : "the disk is read-only");
        } else {
            kdebug("ahci0: another write refused at %u", (unsigned)sector);
        }

        return -1;
    }

    if (!k_volume_write_above_table(&g_disk_id, sector, count)) {
        if (!g_refused) {
            g_refused = true;
            kwarn("ahci0: the write at %u is refused: the table of the partitions lives there",
                  (unsigned)sector);
        }

        return -1;
    }

    /* The writes are a later step of the lane: a volume of this system that
     * /etc/volumes names is the only one the rule lets through, and until
     * the mount can say so the driver has nothing to write. */
    kwarn("ahci0: the write of %u sector(s) at %u is refused: the write path of the "
          "release is not open yet",
          (unsigned)count, (unsigned)sector);

    return -1;
}

/* There is nothing to bring to a medium that is not written: the call exists
 * because the block layer and the partition above it forward a sync. */
static int ahci_sync(void *ctx)
{
    K_UNUSED(ctx);

    return 0;
}

static const struct k_blockdev_ops g_ahci_ops = {
    .read = ahci_read,
    .write = ahci_write,
    .erase = NULL, /* a disk keeps its sectors until they are written */
    .sync = ahci_sync,
};

static struct k_blockdev g_ahci_device = {
    .name = "ahci0",
    .sector_size = MFS_BLOCK_SIZE,
    .sectors = 0, /* the size of the drive, read by the identify */
    .ops = &g_ahci_ops,
    .ctx = NULL,
    .read_ahead = AHCI_READ_AHEAD,
};

/* --- what the rule says about this disk ----------------------------------- */

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

/* --- the boot ------------------------------------------------------------- */

/* The number of the ports the controller implements, for the lines of the
 * log: the register is a bit per port, and 32 is the ceiling of the format. */
static k_u32_t ahci_count_ports(k_u32_t implemented)
{
    k_u32_t count = 0;
    k_u32_t bit;

    for (bit = 0; bit < AHCI_PORTS; bit++) {
        if ((implemented & (1u << bit)) != 0u) {
            count++;
        }
    }

    return count;
}

/*
 * The ABAR of the function and the window of the registers in the kernel map:
 * the BAR is a memory window (not a range of ports), 64 bits when its type
 * says so, and it sits above the identity map of the first gigabyte - the
 * framebuffer of the VGA and the APIC are mapped the same way (ADR 0023).
 *
 * The answer is the address of the registers the driver reads, NULL when the
 * machine has no window the port can use.
 */
static volatile k_u32_t *ahci_map(const struct port_x86_64_pci_device *card)
{
    k_u32_t bar = port_x86_64_pci_read32(card, PCI_BAR5);
    k_u64_t base;
    k_u64_t offset;
    k_u64_t window;

    /* The lowest bit of a BAR says the window is a range of ports; this one
     * is the window of the registers in the memory. The three bits after it
     * are the type of a memory window (32 or 64 bits) and are not the
     * address. */
    if ((bar & PCI_BAR_IO) != 0u) {
        return NULL;
    }

    base = bar & ~(k_u64_t)PCI_BAR_MEMORY_MASK;

    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        base |= (k_u64_t)port_x86_64_pci_read32(card, PCI_BAR6) << 32;
    }

    if (base == 0u) {
        return NULL;
    }

    offset = base & 0x1FFFFFull;
    window = base - offset;

    if (port_x86_64_paging_map_mmio(PORT_X86_64_AHCI_VIRT, window, (k_size_t)(offset + 0x2000u)) ==
        0u) {
        kwarn("ahci: the registers at 0x%x cannot be mapped", (unsigned)base);
        return NULL;
    }

    return (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_AHCI_VIRT + offset);
}

/*
 * The controller, the port with a disk on it, and the disk. A machine without
 * the function, without a disk, or with a disk that is not an ATA drive (a
 * CD-ROM of the SATA channel has another signature) registers nothing and
 * the boot continues on the medium it had.
 */
void port_x86_64_ahci_init(void)
{
    const struct k_blockdev *disk;
    const struct k_blockdev *volume;
    const char *first;
    int partitions;
    k_u32_t port;

    if (!port_x86_64_pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROG_IF_AHCI,
                                    &g_card)) {
        return;
    }

    g_hba = ahci_map(&g_card);

    if (g_hba == NULL) {
        return;
    }

    port_x86_64_pci_enable(&g_card, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    /* AHCI mode, and no interrupt of the controller for a moment: the driver
     * polls the ports and a line that fires without a handler is a storm. */
    reg_write(AHCI_GHC, reg_read(AHCI_GHC) | AHCI_GHC_AE);
    reg_write(AHCI_GHC, reg_read(AHCI_GHC) & ~0x2u);

    g_ports = reg_read(AHCI_PI);

    kinfo("ahci0: %u port(s) of the controller, AHCI %u.%u, %s addressing",
          (unsigned)ahci_count_ports(g_ports), (unsigned)(reg_read(AHCI_VS) >> 16),
          (unsigned)((reg_read(AHCI_VS) >> 8) & 0xFFu),
          (reg_read(AHCI_CAP) & AHCI_CAP_S64A) != 0u ? "64-bit" : "32-bit");

    for (port = 0; port < AHCI_PORTS && port < 32u; port++) {
        if ((g_ports & (1u << port)) == 0u) {
            continue;
        }

        if ((port_read(port, AHCI_P_SSTS) & AHCI_P_SSTS_DET_MASK) != AHCI_P_SSTS_DET_READY) {
            continue;
        }

        if (port_read(port, AHCI_P_SIG) != AHCI_P_SIG_ATA) {
            kinfo("ahci: the port %u carries a device that is not an ATA disk, not used",
                  (unsigned)port);
            continue;
        }

        g_port = port;
        break;
    }

    if (port >= AHCI_PORTS || port >= 32u) {
        kinfo("ahci: no disk on the controller, nothing is registered");
        return;
    }

    if (!ahci_port_start()) {
        kerror("ahci0: the port %u cannot be started", (unsigned)g_port);
        return;
    }

    if (!ahci_identify(&g_medium_sectors)) {
        kerror("ahci0: the disk of the port %u did not identify itself", (unsigned)g_port);
        return;
    }

    if (g_medium_sectors < AHCI_SECTORS_PER_BLOCK ||
        (g_medium_sectors % AHCI_SECTORS_PER_BLOCK) != 0u) {
        kerror("ahci0: the drive is not a whole number of %u-byte blocks",
               (unsigned)MFS_BLOCK_SIZE);
        return;
    }

    g_sectors = g_medium_sectors / AHCI_SECTORS_PER_BLOCK;
    g_ahci_device.sectors = g_sectors;

    /*
     * The cache of the block layer, write-through: a disk of this boot is
     * read-only and a write has to come back as the refusal of the driver,
     * not as a sector that sat in the cache until a sync nobody asked for.
     */
    if (k_blockdev_register_cached_sized(&g_ahci_device, false, AHCI_CACHE_SECTORS) != 0) {
        kerror("ahci0: cannot register the disk");
        return;
    }

    kinfo("ahci0: %u MB on the port %u of %u", (unsigned)(g_medium_sectors / 2048u),
          (unsigned)g_port, (unsigned)ahci_count_ports(g_ports));

    /* The view the table hands out is the one with the cache above it, and
     * the rule reads the same bytes a mount would read. */
    disk = k_blockdev_find(g_ahci_device.name);

    if (disk == NULL) {
        disk = &g_ahci_device;
    }

    /*
     * The whole disk first: its table says where the writes may not go, and
     * the reader of the rule keeps that boundary for the driver. The volume
     * is the partition the boot will mount, so the superblock the rule reads
     * is the one the filesystem is about to read.
     */
    if (k_volume_identify(disk, &g_disk_id) != 0) {
        kerror("ahci0: the head of the disk cannot be read, it is not used");
        return;
    }

    partitions = partition_scan(disk);

    if (partitions < 0 && partitions != KERR_NOENT) {
        kerror("ahci0: the partition table is not usable (%d)", partitions);
    }

    first = partitions > 0 ? partition_name(disk, 0) : NULL;
    volume = first != NULL ? k_blockdev_find(first) : disk;

    if (volume == NULL) {
        volume = disk;
    }

    if (k_volume_identify(volume, &g_id) != 0) {
        kerror("ahci0: the head of %s cannot be read, it is not used",
               volume->name != NULL ? volume->name : "the disk");
        return;
    }

    g_read_only = !k_volume_may_write(volume, &g_id, &g_refusal);

    kinfo("ahci0: %s: %s (%s), %s", volume->name != NULL ? volume->name : "the disk",
          volume_kind_name(&g_id), g_id.format != NULL ? g_id.format : "no superblock",
          g_read_only ? "read-only" : "writable");

    if (g_read_only) {
        kinfo("ahci0: the disk is read-only: %s",
              g_refusal != NULL ? g_refusal : "the rule of ADR 0041");
    }

    if (partitions == KERR_NOENT) {
        kinfo("ahci0: no partition table, the whole disk would be the volume");
    } else if (first == NULL) {
        kinfo("ahci0: the table names no partition this kernel reads");
    } else {
        g_root = first;
        kinfo("x86-64: the root volume is %s (%d partition(s))", first, partitions);
    }

    /* The sentence of the first boot with a new controller: what was found,
     * and that nothing was written. */
    if (g_read_only) {
        kinfo("ahci: 1 disk(s) on the controller, read-only, nothing written");
    } else {
        kinfo("ahci: 1 disk(s) on the controller, writable");
    }
}

const char *port_x86_64_ahci_root(void)
{
    return g_root;
}
