/*
 * Micro OS — the x86-64 port: a stick of the USB as a disk, on the class of
 * the Bulk-Only Transport (ADR 0040, ADR 0041).
 *
 * A stick is a SCSI disk behind a USB interface, and the interface of the
 * mass storage class with the protocol 0x50 (Bulk-Only Transport) moves the
 * commands of that disk over two bulk endpoints. One command is three
 * transfers: a Command Block Wrapper of 31 bytes to the device, the data of
 * the command (a read or a write) in the direction the CDB says, and a
 * Command Status Wrapper of 13 bytes back. The signatures are the proof that
 * the three belong to one command - "USBC" out, "USBS" back, the same tag,
 * and a status of zero - and everything else is the SCSI of a disk:
 * INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ(10) and WRITE(10).
 *
 * This file is the class driver: it knows the descriptors, the commands and
 * the block device, and it asks `src/xhci.c` for the three transfers. The
 * medium behind it is the one of a person, so the rule of ADR 0041 is the
 * first thing the attach does: the disk is registered read-only, the whole
 * disk and its first partition are identified by `kernel/volume.c`, and a
 * write is refused with the sentence of the rule (a volume this system did
 * not make) or with the one of the table (a sector below the first
 * partition). The write path itself is behind the same closed door the AHCI
 * driver keeps: opening it is the `mount -w` of M2 and not a consequence of
 * a mount.
 *
 * The unit of the block layer is 1 KiB, as on the two other disks of the
 * port, so one sector of the filesystem is two of the medium's 512 bytes and
 * the arithmetic of the two lives here.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/partition.h"
#include "kernel/port/time.h"
#include "kernel/volume.h"

#include "mfs/mfs.h"

#include "port_x86_64_xhci.h"

/* --- the command of the Bulk-Only Transport -------------------------------- */

#define BOT_CBW_SIZE 31u
#define BOT_CSW_SIZE 13u
#define BOT_TIMEOUT_MS 3000u

#define BOT_CBW_SIGNATURE 0x43425355u /* "USBC" */
#define BOT_CSW_SIGNATURE 0x53425355u /* "USBS" */

#define BOT_CBW_FLAG_IN 0x80u

#define BOT_STATUS_PASSED 0u
#define BOT_STATUS_FAILED 1u

/* The commands of the SCSI a stick answers (the CDB of the standard). */
#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE 0x03u
#define SCSI_INQUIRY 0x12u
#define SCSI_READ_CAPACITY 0x25u
#define SCSI_READ10 0x28u
#define SCSI_WRITE10 0x2Au
#define SCSI_SYNCHRONIZE_CACHE 0x35u

#define SCSI_SENSE_SIZE 18u
#define SCSI_INQUIRY_SIZE 36u
#define SCSI_CAPACITY_SIZE 8u

/* The bytes of one command of this driver: a read of the block layer is cut
 * into runs of 32 KiB, which is eight pieces of a page and eight TRBs of the
 * ring of the endpoint - a transfer that never wraps the ring. */
#define USB_TRANSFER_MAX 32768u

#define USB_STORAGE_CACHE 128u
#define USB_STORAGE_READ_AHEAD 32u

/* The attempts of TEST UNIT READY: a stick that was just powered answers
 * "not ready" for a moment while its medium spins up (or, in QEMU, while the
 * image is opened). */
#define USB_READY_ATTEMPTS 20u
#define USB_READY_NAP_MS 100u

struct usb_storage {
    struct port_x86_64_usb_device *device;
    k_u8_t lun;
    k_u32_t tag;
    k_u32_t medium_sectors; /* the sectors of 512 bytes of the medium */
    k_u32_t sectors;        /* the blocks of 1 KiB of the block layer */
    bool read_only;
    const char *refusal;
    bool refused;
    bool write_open;
    struct k_volume_id disk_id;      /* the whole disk: the table of the writes */
    struct k_volume_id volume_id;    /* the partition the boot would mount */
    const struct k_blockdev *volume; /* the partition of the volume of this system */
    char vendor[9];
    char product[17];
    char revision[5];
};

static struct usb_storage g_storage;
static const char *g_root;

static k_u8_t g_cbw[BOT_CBW_SIZE] __attribute__((aligned(64)));
static k_u8_t g_csw[BOT_CSW_SIZE] __attribute__((aligned(64)));
static k_u8_t g_sense[SCSI_SENSE_SIZE] __attribute__((aligned(64)));
static k_u8_t g_inquiry[SCSI_INQUIRY_SIZE] __attribute__((aligned(64)));
static k_u8_t g_capacity[SCSI_CAPACITY_SIZE] __attribute__((aligned(64)));

static k_mutex_t g_usb_lock = K_MUTEX_INITIALIZER;

/* The wait of the boot: the scheduler is not there yet when the controller
 * is enumerated, so a delay is a deadline of the clock and a pause. */
static void storage_delay(k_u32_t milliseconds)
{
    k_u64_t deadline = k_port_uptime_ms() + milliseconds;

    while (k_port_uptime_ms() < deadline) {
        __asm__ volatile("pause");
    }
}

static void put_le32(k_u8_t *to, k_u32_t value)
{
    to[0] = (k_u8_t)(value & 0xFFu);
    to[1] = (k_u8_t)((value >> 8) & 0xFFu);
    to[2] = (k_u8_t)((value >> 16) & 0xFFu);
    to[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static void put_be32(k_u8_t *to, k_u32_t value)
{
    to[0] = (k_u8_t)((value >> 24) & 0xFFu);
    to[1] = (k_u8_t)((value >> 16) & 0xFFu);
    to[2] = (k_u8_t)((value >> 8) & 0xFFu);
    to[3] = (k_u8_t)(value & 0xFFu);
}

static void put_be16(k_u8_t *to, k_u16_t value)
{
    to[0] = (k_u8_t)((value >> 8) & 0xFFu);
    to[1] = (k_u8_t)(value & 0xFFu);
}

static k_u32_t get_be32(const k_u8_t *from)
{
    return ((k_u32_t)from[0] << 24) | ((k_u32_t)from[1] << 16) | ((k_u32_t)from[2] << 8) |
           (k_u32_t)from[3];
}

static k_u32_t get_le32(const k_u8_t *from)
{
    return (k_u32_t)from[0] | ((k_u32_t)from[1] << 8) | ((k_u32_t)from[2] << 16) |
           ((k_u32_t)from[3] << 24);
}

/* --- one command ----------------------------------------------------------- */

/*
 * The three transfers of a command. `data` is the buffer of the command (the
 * bytes of the SCSI operation) and `length` is how many of them the device
 * is asked for; a command with no data stage moves the wrapper and the
 * status only.
 *
 * The answer is false when the controller did not move the bytes or the
 * status of the device is not zero: the caller reads the sense of the device
 * before it uses the two wrappers of the next command.
 */
static bool bot_command(const k_u8_t *cdb, k_u8_t cdb_length, void *data, k_u32_t length, bool in,
                        k_u32_t *residue)
{
    k_u32_t tag = ++g_storage.tag;
    k_u32_t moved = 0;
    bool ok = true;

    if (g_storage.device == NULL || cdb == NULL || cdb_length == 0u || cdb_length > 16u) {
        return false;
    }

    memset(g_cbw, 0, sizeof(g_cbw));
    put_le32(g_cbw + 0u, BOT_CBW_SIGNATURE);
    put_le32(g_cbw + 4u, tag);
    put_le32(g_cbw + 8u, length);
    g_cbw[12] = in ? BOT_CBW_FLAG_IN : 0u;
    g_cbw[13] = g_storage.lun;
    g_cbw[14] = cdb_length;
    memcpy(g_cbw + 15u, cdb, cdb_length);

    if (port_x86_64_xhci_bulk(g_storage.device, false, g_cbw, sizeof(g_cbw), BOT_TIMEOUT_MS,
                              &moved) != 0 ||
        moved != sizeof(g_cbw)) {
        return false;
    }

    if (length != 0u) {
        if (port_x86_64_xhci_bulk(g_storage.device, in, data, length, BOT_TIMEOUT_MS, &moved) !=
            0) {
            ok = false;
        } else if (in && moved > length) {
            ok = false;
        }
    }

    if (port_x86_64_xhci_bulk(g_storage.device, true, g_csw, sizeof(g_csw), BOT_TIMEOUT_MS,
                              &moved) != 0 ||
        moved != sizeof(g_csw)) {
        return false;
    }

    if (get_le32(g_csw + 0u) != BOT_CSW_SIGNATURE || get_le32(g_csw + 4u) != tag ||
        g_csw[12] != BOT_STATUS_PASSED) {
        ok = false;
    }

    if (residue != NULL) {
        *residue = get_le32(g_csw + 8u);
    }

    return ok;
}

/* The sense of a command that failed: the request clears the state of the
 * device, so the command after it is a command and not a stall. */
static void bot_clear_halt(void)
{
    k_u8_t cdb[6];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = SCSI_SENSE_SIZE;

    (void)bot_command(cdb, sizeof(cdb), g_sense, SCSI_SENSE_SIZE, true, NULL);
}

/* --- the commands of the SCSI ---------------------------------------------- */

static bool scsi_test_unit_ready(void)
{
    k_u8_t cdb[6];

    memset(cdb, 0, sizeof(cdb));

    return bot_command(cdb, sizeof(cdb), NULL, 0u, false, NULL);
}

static bool scsi_inquiry(void)
{
    k_u8_t cdb[6];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = SCSI_INQUIRY_SIZE;

    if (!bot_command(cdb, sizeof(cdb), g_inquiry, SCSI_INQUIRY_SIZE, true, NULL)) {
        return false;
    }

    /* The peripheral qualifier and the type: 0 is a disk with a medium. */
    if ((g_inquiry[0] & 0x1Fu) != 0u) {
        return false;
    }

    memcpy(g_storage.vendor, g_inquiry + 8u, 8u);
    memcpy(g_storage.product, g_inquiry + 16u, 16u);
    memcpy(g_storage.revision, g_inquiry + 32u, 4u);

    return true;
}

static bool scsi_read_capacity(k_u32_t *sectors, k_u32_t *block_size)
{
    k_u8_t cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY;

    if (!bot_command(cdb, sizeof(cdb), g_capacity, SCSI_CAPACITY_SIZE, true, NULL)) {
        return false;
    }

    /* The last addressable sector and the bytes of one of them: the answer
     * is the *last* LBA, so the count is one more. */
    *sectors = get_be32(g_capacity) + 1u;
    *block_size = get_be32(g_capacity + 4u);

    return *sectors != 0u && *block_size != 0u;
}

/* One READ(10) or WRITE(10) of `blocks` sectors of the medium at `lba`. */
static bool scsi_rw10(bool write, k_u32_t lba, k_u16_t blocks, void *data, k_u32_t length)
{
    k_u8_t cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    put_be32(cdb + 2u, lba);
    put_be16(cdb + 7u, blocks);

    return bot_command(cdb, sizeof(cdb), data, length, !write, NULL);
}

/* The cache of the stick on the medium: SYNCHRONIZE CACHE(10), no data. */
static bool scsi_synchronize_cache(void)
{
    k_u8_t cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SYNCHRONIZE_CACHE;

    return bot_command(cdb, sizeof(cdb), NULL, 0u, false, NULL);
}

/* --- the block device ------------------------------------------------------ */

static bool usb_range_ok(k_u32_t sector, k_size_t count)
{
    return g_storage.sectors != 0u && sector < g_storage.sectors &&
           count <= (k_size_t)g_storage.sectors - sector;
}

/*
 * One read of the block layer: a sector of the filesystem is two of the
 * medium, and a command of the device moves at most 32 KiB - sixteen sectors
 * of the medium and eight of the block layer. The lock is held for the whole
 * run: one set of wrappers is one command, and a second task may ask for its
 * sector between two commands of this one.
 */
static int usb_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t lba;
    k_size_t bytes;
    int rc = 0;

    K_UNUSED(ctx);

    if (buf == NULL || !usb_range_ok(sector, count)) {
        return -1;
    }

    if (k_mutex_lock(&g_usb_lock) != 0) {
        return -1;
    }

    lba = sector * (MFS_BLOCK_SIZE / 512u);
    bytes = count * MFS_BLOCK_SIZE;

    while (bytes > 0u) {
        k_size_t chunk = bytes > USB_TRANSFER_MAX ? USB_TRANSFER_MAX : bytes;

        if (!scsi_rw10(false, lba, (k_u16_t)(chunk / 512u), dst, (k_u32_t)chunk)) {
            kerror("usb0: the read of %u byte(s) at %u failed", (unsigned)chunk, (unsigned)lba);
            bot_clear_halt();
            rc = -1;
            break;
        }

        lba += (k_u32_t)(chunk / 512u);
        dst += chunk;
        bytes -= chunk;
    }

    k_mutex_unlock(&g_usb_lock);

    return rc;
}

/*
 * The write the rule of ADR 0041 refuses, with the two sentences of the AHCI
 * driver: the disk was not made by this system (the superblock and the list
 * of /etc/volumes), and the request names the table of the partitions. The
 * command of the medium exists (WRITE(10)) and the door to it is the one of
 * M2: a mount that asks for the write. Until then every write is refused
 * here, before a byte of the command is built.
 */
static int usb_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(buf);

    if (!usb_range_ok(sector, count)) {
        return -1;
    }

    if (g_storage.read_only) {
        if (!g_storage.refused) {
            g_storage.refused = true;
            kwarn("usb0: the write of %u sector(s) at %u is refused: %s", (unsigned)count,
                  (unsigned)sector,
                  g_storage.refusal != NULL ? g_storage.refusal : "the disk is read-only");
        } else {
            kdebug("usb0: another write refused at %u", (unsigned)sector);
        }

        return -1;
    }

    if (!k_volume_write_above_table(&g_storage.disk_id, sector, count)) {
        if (!g_storage.refused) {
            g_storage.refused = true;
            kwarn("usb0: the write at %u is refused: the table of the partitions lives there",
                  (unsigned)sector);
        }

        return -1;
    }

    if (!g_storage.write_open) {
        kwarn("usb0: the write of %u sector(s) at %u is refused: the write path of the "
              "release is not open yet",
              (unsigned)count, (unsigned)sector);

        return -1;
    }

    /*
     * The door of M2. It is here, complete, because a driver that cannot
     * write a volume the rule names is a driver the rule cannot trust; the
     * flag is off and the mount that asks is a lane of its own.
     */
    if (k_mutex_lock(&g_usb_lock) != 0) {
        return -1;
    }

    {
        const k_u8_t *source = (const k_u8_t *)buf;
        k_u32_t lba = sector * (MFS_BLOCK_SIZE / 512u);
        k_size_t bytes = count * MFS_BLOCK_SIZE;
        int rc = 0;

        while (bytes > 0u) {
            k_size_t chunk = bytes > USB_TRANSFER_MAX ? USB_TRANSFER_MAX : bytes;

            if (!scsi_rw10(true, lba, (k_u16_t)(chunk / 512u), (void *)source, (k_u32_t)chunk)) {
                bot_clear_halt();
                rc = -1;
                break;
            }

            lba += (k_u32_t)(chunk / 512u);
            source += chunk;
            bytes -= chunk;
        }

        k_mutex_unlock(&g_usb_lock);

        return rc;
    }
}

/*
 * A sync of a stick that is written is the SCSI flush of its cache: the
 * command of the class that turns the sectors of the driver into sectors of
 * the medium. A stick that was not opened writes nothing and needs no command.
 */
static int usb_sync(void *ctx)
{
    int rc = 0;

    K_UNUSED(ctx);

    if (!g_storage.write_open) {
        return 0;
    }

    if (k_mutex_lock(&g_usb_lock) != 0) {
        return -1;
    }

    if (!scsi_synchronize_cache()) {
        bot_clear_halt();
        rc = -1;
    }

    k_mutex_unlock(&g_usb_lock);

    return rc;
}

/*
 * The door of the writes of the stick, the one the AHCI driver keeps: a mount
 * that asks for them and the rule of ADR 0041, read again because
 * `/etc/volumes` lives in the volume that is being mounted (ADR 0056). The
 * partition the rule names is the one the kernel took for a root - `usb0p2`
 * of a stick of this system - and not the whole disk.
 */
static int usb_writes(void *ctx, bool open, const char **reason)
{
    const char *why = NULL;

    K_UNUSED(ctx);

    if (!open) {
        g_storage.write_open = false;
        return 0;
    }

    if (g_storage.volume == NULL) {
        if (reason != NULL) {
            *reason = "no USB volume was identified";
        }

        return KERR_ROFS;
    }

    if (k_volume_identify(g_storage.volume, &g_storage.volume_id) != 0 ||
        !k_volume_may_write(g_storage.volume, &g_storage.volume_id, &why)) {
        if (reason != NULL) {
            *reason = why != NULL ? why : "the volume rule refused the writes";
        }

        if (!g_storage.refused) {
            g_storage.refused = true;
            kwarn("usb0: the writes of %s are refused: %s",
                  g_storage.volume->name != NULL ? g_storage.volume->name : "the stick",
                  why != NULL ? why : "the rule of ADR 0041");
        }

        g_storage.read_only = true;
        return KERR_ROFS;
    }

    g_storage.read_only = false;
    g_storage.write_open = true;
    kinfo("usb0: the writes of %s are open: the volume is named in " K_VOLUME_LIST_PATH,
          g_storage.volume->name != NULL ? g_storage.volume->name : "the stick");

    return 0;
}

static const struct k_blockdev_ops g_usb_ops = {
    .read = usb_read,
    .write = usb_write,
    .erase = NULL,
    .sync = usb_sync,
    .writes = usb_writes,
};

static struct k_blockdev g_usb_device = {
    .name = "usb0",
    .sector_size = MFS_BLOCK_SIZE,
    .sectors = 0,
    .ops = &g_usb_ops,
    .ctx = NULL,
    .read_ahead = USB_STORAGE_READ_AHEAD,
};

/* --- what the rule says about this disk ----------------------------------- */

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

/* The eight bytes of a field of the INQUIRY with the spaces taken off. */
static void trim(char *text, k_size_t size)
{
    k_size_t end = size;

    while (end > 0u && (text[end - 1u] == ' ' || text[end - 1u] == '\0')) {
        end--;
    }

    text[end] = '\0';

    while (text[0] == ' ') {
        memmove(text, text + 1, end);
        end--;
    }
}

/* --- the attach of the class ---------------------------------------------- */

/*
 * The configuration of a stick: the interface of class 8, sub-class 6 and
 * protocol 0x50, and the two bulk endpoints that follow it. The class does
 * not read the descriptors of any other interface.
 */
static bool find_interface(const struct port_x86_64_usb_config *config, k_u8_t *interface,
                           k_u8_t *bulk_in, k_u8_t *bulk_out, k_u16_t *in_packet,
                           k_u16_t *out_packet, k_u8_t *interval_in, k_u8_t *interval_out)
{
    k_u32_t offset = 0;
    bool in_class = false;

    *bulk_in = 0;
    *bulk_out = 0;
    *in_packet = 0;
    *out_packet = 0;
    *interval_in = 0;
    *interval_out = 0;

    while (offset + 2u <= config->length) {
        k_u8_t size = config->bytes[offset];
        k_u8_t type = config->bytes[offset + 1u];

        if (size < 2u || offset + size > config->length) {
            return false;
        }

        if (type == 4u && size >= 9u) {
            in_class = config->bytes[offset + 5u] == 8u && config->bytes[offset + 6u] == 6u &&
                       config->bytes[offset + 7u] == 0x50u;

            if (in_class) {
                *interface = config->bytes[offset + 2u];
            }
        } else if (type == 5u && size >= 7u && in_class) {
            k_u8_t address = config->bytes[offset + 2u];
            k_u8_t attributes = config->bytes[offset + 3u];
            k_u16_t packet =
                (k_u16_t)(config->bytes[offset + 4u] | ((k_u16_t)config->bytes[offset + 5u] << 8));
            k_u8_t interval = config->bytes[offset + 6u];

            if ((attributes & 0x03u) == 0x02u) {
                if ((address & 0x80u) != 0u) {
                    *bulk_in = address;
                    *in_packet = packet;
                    *interval_in = interval;
                } else {
                    *bulk_out = address;
                    *out_packet = packet;
                    *interval_out = interval;
                }
            }
        }

        offset += size;
    }

    return in_class && *bulk_in != 0u && *bulk_out != 0u && *in_packet != 0u && *out_packet != 0u;
}

bool port_x86_64_usb_storage_attach(struct port_x86_64_usb_device *device,
                                    const struct port_x86_64_usb_config *config)
{
    const struct k_blockdev *disk;
    const struct k_blockdev *volume;
    const struct k_blockdev *ours = NULL;
    struct k_volume_id ours_id;
    const char *first;
    k_u8_t interface = 0;
    k_u8_t bulk_in = 0;
    k_u8_t bulk_out = 0;
    k_u8_t interval_in = 0;
    k_u8_t interval_out = 0;
    k_u16_t in_packet = 0;
    k_u16_t out_packet = 0;
    k_u32_t medium_sectors = 0;
    k_u32_t block_size = 0;
    k_u32_t attempt;
    int partitions;

    if (device == NULL || config == NULL || config->bytes == NULL) {
        return false;
    }

    if (!find_interface(config, &interface, &bulk_in, &bulk_out, &in_packet, &out_packet,
                        &interval_in, &interval_out)) {
        kinfo("usb: port %u: the storage interface has no two bulk endpoints",
              (unsigned)device->port);
        return false;
    }

    if (port_x86_64_xhci_endpoint(device, bulk_out, true, interval_out, out_packet) != 0 ||
        port_x86_64_xhci_endpoint(device, bulk_in, true, interval_in, in_packet) != 0) {
        kinfo("usb: port %u: the endpoints of the stick did not come up", (unsigned)device->port);
        return false;
    }

    memset(&g_storage, 0, sizeof(g_storage));
    g_storage.device = device;
    g_storage.lun = 0;
    device->interface = interface;

    if (!scsi_inquiry()) {
        kinfo("usb: port %u: the stick did not answer INQUIRY", (unsigned)device->port);
        bot_clear_halt();
        return false;
    }

    trim(g_storage.vendor, sizeof(g_storage.vendor) - 1u);
    trim(g_storage.product, sizeof(g_storage.product) - 1u);
    trim(g_storage.revision, sizeof(g_storage.revision) - 1u);

    /* A stick that was just attached answers "not ready" for a moment. */
    for (attempt = 0; attempt < USB_READY_ATTEMPTS; attempt++) {
        if (scsi_test_unit_ready()) {
            break;
        }

        bot_clear_halt();
        storage_delay(USB_READY_NAP_MS);
    }

    if (attempt == USB_READY_ATTEMPTS) {
        kinfo("usb: port %u: the stick is not ready", (unsigned)device->port);
        return false;
    }

    if (!scsi_read_capacity(&medium_sectors, &block_size)) {
        kinfo("usb: port %u: the stick did not answer READ CAPACITY", (unsigned)device->port);
        bot_clear_halt();
        return false;
    }

    if (block_size != 512u || medium_sectors < 2u || (medium_sectors % 2u) != 0u) {
        kinfo("usb: port %u: the stick is not a whole number of %u-byte blocks",
              (unsigned)device->port, (unsigned)MFS_BLOCK_SIZE);
        return false;
    }

    g_storage.medium_sectors = medium_sectors;
    g_storage.sectors = medium_sectors / 2u;
    g_usb_device.sectors = g_storage.sectors;

    /*
     * The cache of the block layer, write-through: the disk of this boot is
     * read-only, and a write has to come back as the refusal of the driver,
     * not as a sector that sat in the cache until a sync nobody asked for.
     */
    if (k_blockdev_register_cached_sized(&g_usb_device, false, USB_STORAGE_CACHE) != 0) {
        kerror("usb0: cannot register the disk");
        return false;
    }

    kinfo("usb0: %s %s %s, %u MB on the port %u of the controller",
          g_storage.vendor[0] != '\0' ? g_storage.vendor : "a stick", g_storage.product,
          g_storage.revision, (unsigned)(g_storage.medium_sectors / 2048u), (unsigned)device->port);

    disk = k_blockdev_find(g_usb_device.name);

    if (disk == NULL) {
        disk = &g_usb_device;
    }

    if (k_volume_identify(disk, &g_storage.disk_id) != 0) {
        kerror("usb0: the head of the disk cannot be read, it is not used");
        return false;
    }

    partitions = partition_scan(disk);

    if (partitions < 0 && partitions != KERR_NOENT) {
        kerror("usb0: the partition table is not usable (%d)", partitions);
    }

    /*
     * The partitions of the stick, one at a time. The first is the one the
     * rule of ADR 0041 has always named and the one a person mounts; the one
     * that carries a volume of this system is the root the kernel prefers
     * over the image of the memory (ADR 0056). The ESP of a stick of this
     * system is FAT32 and foreign, and the volume of the machine is the
     * MicroFS partition after it.
     */
    first = partitions > 0 ? partition_name(disk, 0) : NULL;
    volume = first != NULL ? k_blockdev_find(first) : disk;

    if (volume == NULL) {
        volume = disk;
    }

    if (k_volume_identify(volume, &g_storage.volume_id) != 0) {
        kerror("usb0: the head of %s cannot be read, it is not used",
               volume->name != NULL ? volume->name : "the disk");
        return false;
    }

    g_storage.read_only = !k_volume_may_write(volume, &g_storage.volume_id, &g_storage.refusal);

    kinfo("usb0: %s: %s (%s), %s", volume->name != NULL ? volume->name : "the disk",
          volume_kind_name(&g_storage.volume_id),
          g_storage.volume_id.format != NULL ? g_storage.volume_id.format : "no superblock",
          g_storage.read_only ? "read-only" : "writable");

    if (g_storage.read_only) {
        kinfo("usb0: the disk is read-only: %s",
              g_storage.refusal != NULL ? g_storage.refusal : "the rule of ADR 0041");
    }

    g_storage.volume = volume;

    memset(&ours_id, 0, sizeof(ours_id));

    if (partitions > 0) {
        k_size_t i;

        for (i = 0; i < (k_size_t)partitions; i++) {
            const char *name = partition_name(disk, i);
            const struct k_blockdev *part = name != NULL ? k_blockdev_find(name) : NULL;
            struct k_volume_id id;

            if (part == NULL || k_volume_identify(part, &id) != 0) {
                continue;
            }

            /*
             * The other partitions are named one by one: the gate of the
             * stick of ADR 0056 reads the line of the second one, and a
             * person with a medium of three partitions sees what the kernel
             * made of each.
             */
            if (i > 0u) {
                bool ro = !k_volume_may_write(part, &id, NULL);

                kinfo("usb0: %s: %s (%s), %s", part->name, volume_kind_name(&id),
                      id.format != NULL ? id.format : "no superblock",
                      ro ? "read-only" : "writable");
            }

            if (ours == NULL && id.kind == K_VOLUME_OURS) {
                ours = part;
                ours_id = id;
            }
        }
    } else if (g_storage.volume_id.kind == K_VOLUME_OURS) {
        ours = volume;
        ours_id = g_storage.volume_id;
    }

    if (partitions == KERR_NOENT) {
        kinfo("usb0: no partition table, the whole disk would be the volume");
    } else if (first == NULL) {
        kinfo("usb0: the table names no partition this kernel reads");
    } else {
        kinfo("usb0: the first partition is %s (%d partition(s))", first, partitions);
    }

    if (ours != NULL) {
        g_storage.volume = ours;
        g_storage.volume_id = ours_id;
        g_storage.read_only = !k_volume_may_write(ours, &ours_id, &g_storage.refusal);
        g_root = ours->name;

        kinfo("usb0: %s: %s (%s), %s", ours->name, volume_kind_name(&ours_id),
              ours_id.format != NULL ? ours_id.format : "no superblock",
              g_storage.read_only ? "read-only" : "writable");
        kinfo("usb0: the volume of the machine is %s (%d partition(s))", ours->name, partitions);
    } else {
        g_root = NULL;
    }

    if (g_storage.read_only) {
        kinfo("usb: 1 disk(s) of the storage class, read-only, nothing written");
    } else {
        kinfo("usb: 1 disk(s) of the storage class, writable");
    }

    return true;
}

const char *port_x86_64_usb_storage_root(void)
{
    return g_root;
}

/*
 * The stick left. The partitions are given back before the disk, because the
 * children read through the cache of the disk; a medium that is mounted is a
 * case the VFS has no call to ask, and the reads of a volume that lost its
 * device fail with the rule of the block layer instead of a panic.
 */
void port_x86_64_usb_storage_detach(struct port_x86_64_usb_device *device)
{
    const struct k_blockdev *disk;

    if (device == NULL || g_storage.device != device) {
        return;
    }

    disk = k_blockdev_find(g_usb_device.name);

    if (disk != NULL) {
        partition_release(disk);
    }

    (void)k_blockdev_unregister(&g_usb_device);

    kinfo("usb0: the stick on the port %u is gone", (unsigned)device->port);

    g_root = NULL;
    g_usb_device.sectors = 0;
    memset(&g_storage, 0, sizeof(g_storage));
}
