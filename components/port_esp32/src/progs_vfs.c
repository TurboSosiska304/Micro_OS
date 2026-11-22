/*
 * Micro OS — the programs of the board are files of the `progs` partition.
 *
 * ADR 0018 §2 and ADR 0022: a program is a `.mox` in a partition of its own,
 * written there by tools/progs_image.py at build time and flashed with the
 * firmware. This file makes the partition a read-only filesystem mounted at
 * /bin, so the shell lists the programs with `ls`, `execve` opens them
 * through the VFS like a file of any other volume, and the port receives the
 * bytes of the image exactly as it received the blob of 1.0. There is no
 * blob in the firmware and no second path into the loader.
 *
 * The bytes are read with esp_partition_read and not mapped: the loader
 * copies a program into the two windows of the port anyway (ADR 0018 §3),
 * and a mapping would add a fixed virtual address that the first mapping of
 * the boot has to reserve. The directory keeps every file on a 64 KiB
 * boundary, which is the address esp_partition_mmap will need when a later
 * loader takes the flash path; the format does not change for that.
 *
 * The mount is the directory /bin and nothing else: there are no directories
 * below it, no write, no unlink and no chmod. The partition is data of the
 * firmware, not a volume of the user; a program is replaced by a new image
 * and not by `cp` at the shell.
 */
#include "port_esp32.h"

#include <string.h>

#include "esp_partition.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "progs_format.h"

/* The label of the partition in partitions.csv. */
#define PROGS_PARTITION_LABEL "progs"
#define PROGS_MOUNT_POINT "/bin"

/* A file is executable by the one user of the system (ADR 0005). */
#define PROGS_MODE 0755u

/* The private data of an open file: the index of its record and no more. */
struct progs_open {
    k_u32_t index;
    k_u32_t is_directory;
};

static const esp_partition_t *s_partition;
static struct progs_entry s_entries[PROGS_MAX_FILES];
static k_size_t s_entry_count;
static char s_point[VFS_PATH_MAX];

/* The record of a name, or NULL. The table is small and is searched as is. */
static const struct progs_entry *entry_find(const char *name)
{
    k_size_t i;

    for (i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) {
            return &s_entries[i];
        }
    }

    return NULL;
}

/* The name of a path below the mount, or NULL when the path is not there. */
static const char *base_name(const char *path)
{
    k_size_t length = strlen(s_point);
    const char *name;

    if (strncmp(path, s_point, length) != 0) {
        return NULL;
    }

    if (path[length] == '\0') {
        return NULL;
    }

    if (path[length] != '/') {
        return NULL;
    }

    name = path + length + 1u;
    if (name[0] == '\0' || strchr(name, '/') != NULL) {
        return NULL;
    }

    return name;
}

static bool path_is_point(const char *path)
{
    return path != NULL && strcmp(path, s_point) == 0;
}

static int progs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct progs_open *open_file = (struct progs_open *)out->priv;
    const struct progs_entry *entry;
    const char *name;

    K_UNUSED(ctx);

    if (open_file == NULL) {
        return KERR_INVAL;
    }

    if ((flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC)) != 0) {
        return KERR_PERM;
    }

    if (path_is_point(path)) {
        if ((flags & VFS_O_DIRECTORY) == 0) {
            return KERR_ISDIR;
        }

        open_file->index = 0;
        open_file->is_directory = 1;
        out->inode = 0;
        out->size = 0;
        return 0;
    }

    name = base_name(path);
    if (name == NULL) {
        return KERR_NOENT;
    }

    entry = entry_find(name);
    if (entry == NULL) {
        return KERR_NOENT;
    }

    open_file->index = (k_u32_t)(entry - s_entries);
    open_file->is_directory = 0;
    out->inode = open_file->index + 1u;
    out->size = entry->size;

    return 0;
}

static int progs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    const struct progs_open *open_file = (const struct progs_open *)file->priv;
    const struct progs_entry *entry;
    k_size_t want;

    K_UNUSED(ctx);

    if (open_file == NULL || s_partition == NULL || open_file->is_directory != 0) {
        return KERR_INVAL;
    }

    if (open_file->index >= s_entry_count) {
        return KERR_NOENT;
    }

    entry = &s_entries[open_file->index];

    if (file->offset >= entry->size) {
        return 0;
    }

    want = (k_size_t)(entry->size - file->offset);
    if (want > size) {
        want = size;
    }

    if (esp_partition_read(s_partition, (size_t)entry->offset + (size_t)file->offset, buf, want) !=
        ESP_OK) {
        kerror("progs: cannot read %s", entry->name);
        return KERR_IO;
    }

    file->offset += want;

    return (int)want;
}

static int progs_write(void *ctx, struct vfs_file *file, const void *buf, k_size_t size)
{
    K_UNUSED(ctx);
    K_UNUSED(file);
    K_UNUSED(buf);
    K_UNUSED(size);

    return KERR_PERM;
}

static int progs_close(void *ctx, struct vfs_file *file)
{
    K_UNUSED(ctx);
    K_UNUSED(file);

    return 0;
}

static int progs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                      k_size_t *count)
{
    k_size_t found = 0;
    k_size_t i;

    K_UNUSED(ctx);

    if (!path_is_point(path)) {
        return KERR_NOENT;
    }

    for (i = 0; i < s_entry_count && found < max; i++) {
        memset(&out[found], 0, sizeof(out[found]));
        memcpy(out[found].name, s_entries[i].name, sizeof(s_entries[i].name));
        out[found].inode = (k_u32_t)i + 1u;
        out[found].type = 1;
        out[found].size = s_entries[i].size;
        found++;
    }

    *count = found;

    return 0;
}

static int progs_readdir(void *ctx, struct vfs_file *file, struct vfs_dirent *out, k_size_t max,
                         k_size_t *count)
{
    struct progs_open *open_file = (struct progs_open *)file->priv;
    k_size_t found = 0;
    k_size_t index;

    K_UNUSED(ctx);

    if (open_file == NULL || open_file->is_directory == 0) {
        return KERR_NOTDIR;
    }

    index = (k_size_t)file->offset;
    while (index < s_entry_count && found < max) {
        memset(&out[found], 0, sizeof(out[found]));
        memcpy(out[found].name, s_entries[index].name, sizeof(s_entries[index].name));
        out[found].inode = (k_u32_t)index + 1u;
        out[found].type = 1;
        out[found].size = s_entries[index].size;
        index++;
        found++;
    }

    file->offset = index;
    *count = found;

    return 0;
}

static int progs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    const struct progs_entry *entry;
    const char *name;

    K_UNUSED(ctx);

    if (path_is_point(path)) {
        out->type = 2;
        return 0;
    }

    name = base_name(path);
    if (name == NULL) {
        return KERR_NOENT;
    }

    entry = entry_find(name);
    if (entry == NULL) {
        return KERR_NOENT;
    }

    out->inode = (k_u32_t)(entry - s_entries) + 1u;
    out->type = 1;
    out->size = entry->size;
    out->mode = PROGS_MODE;
    out->owner = 0;

    return 0;
}

static int progs_statfs(void *ctx, struct vfs_statfs *out)
{
    k_size_t total = 0;
    k_size_t i;

    K_UNUSED(ctx);

    for (i = 0; i < s_entry_count; i++) {
        total += s_entries[i].size;
    }

    out->block_size = 4096;
    out->total_blocks = total / 4096u;
    out->free_blocks = 0;
    out->total_inodes = s_entry_count;
    out->free_inodes = 0;

    return 0;
}

static int progs_permission(void *ctx, const char *path, k_u32_t value)
{
    K_UNUSED(ctx);
    K_UNUSED(path);
    K_UNUSED(value);

    return KERR_PERM;
}

static int progs_mkdir(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    K_UNUSED(path);

    return KERR_PERM;
}

static int progs_unlink(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    K_UNUSED(path);

    return KERR_PERM;
}

static const struct vfs_operations g_progs_operations = {
    .open = progs_open,
    .read = progs_read,
    .write = progs_write,
    .close = progs_close,
    .list = progs_list,
    .readdir = progs_readdir,
    .stat = progs_stat,
    .mkdir = progs_mkdir,
    .unlink = progs_unlink,
    .chmod = progs_permission,
    .chown = progs_permission,
    .statfs = progs_statfs,
    .strerror = NULL,
    .umount = NULL,
};

/*
 * Read the directory of the partition. A table that does not parse is an
 * empty /bin and a line on the console: the boot still reaches the shell,
 * which is where a person can see that the partition was not flashed.
 */
static void progs_scan(void)
{
    k_u8_t table[PROGS_TABLE_HEADER_SIZE + PROGS_MAX_FILES * PROGS_ENTRY_SIZE];
    k_size_t count = 0;

    s_entry_count = 0;

    if (s_partition == NULL) {
        return;
    }

    if (s_partition->size <= PROGS_DIRECTORY_SIZE) {
        kerror("progs: '%s' is too small for its directory", PROGS_PARTITION_LABEL);
        return;
    }

    if (esp_partition_read(s_partition, s_partition->size - PROGS_DIRECTORY_SIZE, table,
                           sizeof(table)) != ESP_OK) {
        kerror("progs: the directory of '%s' is not readable", PROGS_PARTITION_LABEL);
        return;
    }

    if (progs_directory_parse(table, sizeof(table), s_partition->size, s_entries, PROGS_MAX_FILES,
                              &count) != 0) {
        kwarn("progs: the directory of '%s' is not a table of version %u, /bin is empty",
              PROGS_PARTITION_LABEL, (unsigned)PROGS_VERSION);
        return;
    }

    s_entry_count = count;

    kinfo("progs: %u program(s) in '%s'", (unsigned)s_entry_count, PROGS_PARTITION_LABEL);
}

static int progs_mount_type(const char *point, const char *device)
{
    K_UNUSED(device);

    if (s_partition == NULL) {
        s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                               PROGS_PARTITION_LABEL);
        if (s_partition == NULL) {
            kerror("progs: the partition '%s' is not in the table", PROGS_PARTITION_LABEL);
            return KERR_NOENT;
        }

        if (s_partition->address % PROGS_SLOT_SIZE != 0) {
            /* The flash window of a program starts on a 64 KiB boundary of
             * the flash; a partition that is not aligned cannot be mapped. */
            kwarn("progs: '%s' starts at 0x%x, not on a 64 KiB boundary", PROGS_PARTITION_LABEL,
                  (unsigned)s_partition->address);
        }

        progs_scan();
    }

    if (strlen(point) >= sizeof(s_point)) {
        return KERR_INVAL;
    }

    if (vfs_mount(point, &g_progs_operations, NULL) != 0) {
        return KERR_BUSY;
    }

    memcpy(s_point, point, strlen(point) + 1u);

    return 0;
}

int port_esp32_progs_mount(void)
{
    int rc;

    rc = vfs_register_type("progs", progs_mount_type);
    if (rc != 0) {
        kerror("progs: the type of the filesystem is not registered (rc=%d)", rc);
        return rc;
    }

    rc = vfs_mount_type(PROGS_MOUNT_POINT, "progs", PROGS_PARTITION_LABEL);
    if (rc != 0) {
        kerror("progs: cannot mount %s (rc=%d)", PROGS_MOUNT_POINT, rc);
        return rc;
    }

    kinfo("progs: %s is mounted on '%s'", PROGS_MOUNT_POINT, PROGS_PARTITION_LABEL);

    return 0;
}
