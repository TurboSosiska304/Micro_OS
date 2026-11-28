/*
 * Micro OS — the tree and the record of an installed volume.
 *
 * The tree is small on purpose: L3 of docs/plan-install.md creates the
 * filesystem and the places a boot expects, and leaves the copy of the system
 * to the next lane. Every path is written through the VFS, so the same calls
 * serve the installer on x86-64 and the host tests that mount a RAM volume.
 */
#include "kernel/install.h"

#include <stdio.h>
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/clock.h"
#include "kernel/errno.h"
#include "kernel/partition.h"
#include "kernel/vfs.h"
#include "kernel/version.h"

#define INSTALL_FILE_DATA VFS_FILE_DATA_SIZE
#define INSTALL_SOURCE "kernel:"

/* The five directories a boot mounts or uses from the root of the volume. */
static const char *const g_mount_points[] = {"dev", "proc", "tmp", "mnt"};

void install_date(char *out, k_size_t size, k_u64_t unix_seconds)
{
    k_u64_t days = unix_seconds / 86400u;
    k_u64_t z = days + 719468u;
    k_u64_t era = z / 146097u;
    k_u64_t doe = z - era * 146097u;
    k_u64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    k_u64_t year = yoe + era * 400u;
    k_u64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    k_u64_t mp = (5u * doy + 2u) / 153u;
    k_u64_t day = doy - (153u * mp + 2u) / 5u + 1u;
    k_u64_t month = mp < 10u ? mp + 3u : mp - 9u;

    year += month <= 2u ? 1u : 0u;
    (void)snprintf(out, size, "%04llu-%02llu-%02llu", (unsigned long long)year,
                   (unsigned long long)month, (unsigned long long)day);
}

/* FNV-1a over the identity of the running system. It is the source hash of
 * an install that has not copied a tree yet: the next lane replaces it with
 * the hash of the manifest it verifies. */
static k_u64_t source_hash(void)
{
    const char *const parts[] = {INSTALL_SOURCE, kernel_version(), kernel_build_id(),
                                 kernel_compiler()};
    k_u64_t hash = 1469598103934665603ULL;
    k_size_t i;
    k_size_t j;

    for (i = 0u; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (j = 0u; parts[i][j] != '\0'; j++) {
            hash ^= (k_u8_t)parts[i][j];
            hash *= 1099511628211ULL;
        }
    }

    return hash;
}

static int write_file(const char *path, const char *text)
{
    k_u8_t data[INSTALL_FILE_DATA];
    struct vfs_file file;
    int rc;
    int written;

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data);
    if (rc != 0) {
        return rc;
    }

    written = vfs_write(&file, text, strlen(text));
    rc = vfs_close(&file);

    return written < 0 ? written : rc;
}

static int make_dir(const char *point, const char *name)
{
    char path[VFS_PATH_MAX];

    if (snprintf(path, sizeof(path), "%s/%s", point, name) >= (int)sizeof(path)) {
        return KERR_NAMETOOLONG;
    }

    return vfs_mkdir(path);
}

static void guid_text(const struct install_record *rec, char *out, k_size_t size)
{
    k_u8_t guid[PARTITION_GUID_SIZE];
    k_u32_t a = 0u;
    k_u16_t b = 0u;
    k_u16_t c = 0u;

    partition_guid(rec->start_lba, rec->sector_count, guid);
    memcpy(&a, guid, sizeof(a));
    memcpy(&b, guid + 4u, sizeof(b));
    memcpy(&c, guid + 6u, sizeof(c));
    (void)snprintf(out, size, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)a,
                   (unsigned)b, (unsigned)c, guid[8], guid[9], guid[10], guid[11], guid[12],
                   guid[13], guid[14], guid[15]);
}

int install_tree_create(const char *point, const struct install_record *rec)
{
    char path[VFS_PATH_MAX];
    char record[1024];
    char uuid[64];
    char date[32];
    k_size_t i;
    int rc;

    if (point == NULL || rec == NULL || rec->volume == NULL || rec->fs == NULL ||
        rec->boot_mode == NULL) {
        return KERR_INVAL;
    }

    rc = make_dir(point, "etc");
    if (rc != 0 && rc != KERR_EXIST) {
        return rc;
    }

    for (i = 0u; i < sizeof(g_mount_points) / sizeof(g_mount_points[0]); i++) {
        rc = make_dir(point, g_mount_points[i]);
        if (rc != 0 && rc != KERR_EXIST) {
            return rc;
        }
    }

    if (snprintf(path, sizeof(path), "%s%s", point, INSTALL_VOLUMES_PATH) >= (int)sizeof(path)) {
        return KERR_NAMETOOLONG;
    }
    if (snprintf(record, sizeof(record), "%s\n", rec->volume) >= (int)sizeof(record)) {
        return KERR_NAMETOOLONG;
    }
    rc = write_file(path, record);
    if (rc != 0) {
        return rc;
    }

    guid_text(rec, uuid, sizeof(uuid));
    install_date(date, sizeof(date), k_clock_now_unix());
    (void)snprintf(record, sizeof(record),
                   "version: %s\n"
                   "source: fnv1a64:%016llx\n"
                   "uuid: %s\n"
                   "date: %s\n"
                   "boot: %s\n"
                   "fs: %s\n"
                   "device: %s\n"
                   "size: %llu sector(s)\n",
                   kernel_version(), (unsigned long long)source_hash(), uuid, date, rec->boot_mode,
                   rec->fs, rec->volume, (unsigned long long)rec->sector_count);

    if (snprintf(path, sizeof(path), "%s%s", point, INSTALL_RECORD_PATH) >= (int)sizeof(path)) {
        return KERR_NAMETOOLONG;
    }

    return write_file(path, record);
}
