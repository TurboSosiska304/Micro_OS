/*
 * Micro OS — the ramfs.
 *
 * A filesystem in the memory of the machine. A file takes its blocks from the
 * kernel's allocator as it is written and gives them back when it is removed,
 * so the limit of a file is the memory that is free. The last megabyte of the
 * heap (RAMFS_RESERVE_KB) is not given to a write of a person, which answers
 * ENOSPC instead, so the machine keeps running when /tmp is full.
 *
 * The filesystem is a singleton: one volume, mounted at one point. It is not
 * built on the board, where there is no room for it (the desk of 2.0 is the
 * machine of x86-64).
 */
#include "ramfs/ramfs_vfs.h"

#include <string.h>

#include "kernel/clock.h"
#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/sysinfo.h"
#include "kernel/vfs.h"

#define RAMFS_BLOCK_SIZE 1024u
#define RAMFS_NAME_MAX VFS_NAME_MAX

/* The type of a node, the same numbers the VFS uses in a dirent. */
#define RAMFS_TYPE_FILE 1u
#define RAMFS_TYPE_DIR 2u

/* The reserve of the heap that a write of a person may not take. */
#define RAMFS_RESERVE_BYTES ((k_size_t)RAMFS_RESERVE_KB * 1024u)

struct ramfs_node;

struct ramfs_entry {
    char name[RAMFS_NAME_MAX + 1];
    struct ramfs_node *node;
    struct ramfs_entry *next;
};

struct ramfs_node {
    k_u32_t inode;
    k_u8_t type;
    k_u32_t mode;
    k_u32_t owner;
    k_u32_t group;
    k_u64_t size;
    k_u64_t mtime;
    k_u64_t ctime;
    /*
     * The number of the open descriptors and of the names that reach this
     * node. A node that was unlinked from its directory but is still open
     * keeps its blocks until the last close drops both counts to zero.
     */
    k_u32_t open_count;
    k_u32_t nlink;

    /* A file: blocks of data, allocated as they are written. */
    k_u8_t **blocks;
    k_size_t block_count;
    k_size_t block_cap;

    /* A directory: the entries it holds. */
    struct ramfs_entry *entries;
};

struct ramfs_file {
    struct ramfs_node *node;
    k_u8_t append;
};

_Static_assert(sizeof(struct ramfs_file) <= VFS_FILE_DATA_SIZE,
               "the file of the ramfs does not fit into a descriptor of the VFS");

static k_mutex_t g_lock;
static struct ramfs_node *g_root;
static bool g_mounted;
static k_u32_t g_next_inode = 1;
static char g_point[VFS_PATH_MAX];

static const char *ramfs_relative(const char *path)
{
    k_size_t len;

    if (path == NULL || g_point[0] == '\0') {
        return NULL;
    }

    len = strlen(g_point);
    if (strncmp(path, g_point, len) != 0) {
        return NULL;
    }

    if (len == 1u) {
        return path[0] == '/' ? path : NULL;
    }

    if (path[len] == '\0') {
        return "/";
    }

    if (path[len] != '/') {
        return NULL;
    }

    return path + len;
}

static struct ramfs_node *node_new(k_u8_t type)
{
    struct ramfs_node *node = kmm_alloc(sizeof(*node), KMM_PRIORITY_KERNEL);

    if (node == NULL) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->inode = g_next_inode++;
    node->type = type;
    node->nlink = 1;
    node->mtime = k_clock_now_unix();
    node->ctime = node->mtime;
    if (type == RAMFS_TYPE_DIR) {
        node->mode = 0755u;
    } else {
        node->mode = 0644u;
    }
    return node;
}

static k_size_t heap_free(void)
{
    return k_sysinfo_heap_free();
}

static bool reserve_allows(k_size_t need)
{
    k_size_t free = heap_free();

    if (free < RAMFS_RESERVE_BYTES) {
        return false;
    }

    return (free - RAMFS_RESERVE_BYTES) >= need;
}

static void block_free(k_u8_t *block)
{
    kmm_free(block);
}

static void node_release(struct ramfs_node *node);

static void node_free_contents(struct ramfs_node *node)
{
    k_size_t i;

    if (node == NULL) {
        return;
    }

    if (node->type == RAMFS_TYPE_FILE) {
        for (i = 0; i < node->block_count; i++) {
            if (node->blocks[i] != NULL) {
                block_free(node->blocks[i]);
                node->blocks[i] = NULL;
            }
        }
        kmm_free(node->blocks);
        node->blocks = NULL;
        node->block_count = 0;
        node->block_cap = 0;
        node->size = 0;
    } else {
        struct ramfs_entry *entry = node->entries;

        while (entry != NULL) {
            struct ramfs_entry *next = entry->next;

            if (entry->node != NULL) {
                entry->node->nlink = (entry->node->nlink > 0) ? entry->node->nlink - 1 : 0;
                node_release(entry->node);
            }
            kmm_free(entry);
            entry = next;
        }
        node->entries = NULL;
    }
}

static void node_release(struct ramfs_node *node)
{
    if (node == NULL) {
        return;
    }

    if (node->open_count > 0 || node->nlink > 0) {
        return;
    }

    node_free_contents(node);
    kmm_free(node);
}

static struct ramfs_entry *dir_find(struct ramfs_node *dir, const char *name)
{
    struct ramfs_entry *entry;

    for (entry = dir->entries; entry != NULL; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
    }

    return NULL;
}

static int dir_add(struct ramfs_node *dir, const char *name, struct ramfs_node *node)
{
    struct ramfs_entry *entry;
    k_size_t len = strlen(name);

    if (len == 0 || len > RAMFS_NAME_MAX) {
        return KERR_INVAL;
    }

    if (dir_find(dir, name) != NULL) {
        return KERR_EXIST;
    }

    if (!reserve_allows(sizeof(*entry))) {
        return KERR_NOSPC;
    }

    entry = kmm_alloc(sizeof(*entry), KMM_PRIORITY_KERNEL);
    if (entry == NULL) {
        return KERR_NOSPC;
    }

    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, len + 1);
    entry->node = node;
    entry->next = dir->entries;
    dir->entries = entry;
    node->nlink++;
    dir->mtime = k_clock_now_unix();
    dir->ctime = dir->mtime;
    return 0;
}

static int dir_remove(struct ramfs_node *dir, const char *name, struct ramfs_node **removed)
{
    struct ramfs_entry *entry;
    struct ramfs_entry *prev = NULL;

    for (entry = dir->entries; entry != NULL; prev = entry, entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            if (prev == NULL) {
                dir->entries = entry->next;
            } else {
                prev->next = entry->next;
            }
            if (removed != NULL) {
                *removed = entry->node;
            }
            if (entry->node != NULL) {
                entry->node->nlink = (entry->node->nlink > 0) ? entry->node->nlink - 1 : 0;
            }
            kmm_free(entry);
            dir->mtime = k_clock_now_unix();
            dir->ctime = dir->mtime;
            return 0;
        }
    }

    return KERR_NOENT;
}

static bool dir_is_empty(struct ramfs_node *dir)
{
    return dir == NULL || dir->entries == NULL;
}

/* Resolve a path to its node. Returns NULL on failure and sets *err. */
static struct ramfs_node *resolve_node(const char *path, int *err)
{
    const char *p;
    char part[RAMFS_NAME_MAX + 1];
    struct ramfs_node *node;

    if (err != NULL) {
        *err = KERR_NOENT;
    }

    if (path == NULL || path[0] != '/' || g_root == NULL) {
        if (err != NULL) {
            *err = KERR_INVAL;
        }
        return NULL;
    }

    if (strcmp(path, "/") == 0) {
        return g_root;
    }

    node = g_root;
    p = path;
    while (*p == '/') {
        p++;
    }

    while (*p != '\0') {
        k_size_t len = 0;
        struct ramfs_entry *entry;

        while (p[len] != '\0' && p[len] != '/') {
            if (len >= RAMFS_NAME_MAX) {
                if (err != NULL) {
                    *err = KERR_NAMETOOLONG;
                }
                return NULL;
            }
            part[len] = p[len];
            len++;
        }
        part[len] = '\0';
        p += len;
        while (*p == '/') {
            p++;
        }

        if (part[0] == '\0') {
            continue;
        }

        if (node->type != RAMFS_TYPE_DIR) {
            if (err != NULL) {
                *err = KERR_NOTDIR;
            }
            return NULL;
        }

        entry = dir_find(node, part);
        if (entry == NULL) {
            if (err != NULL) {
                *err = KERR_NOENT;
            }
            return NULL;
        }
        node = entry->node;
    }

    return node;
}

/*
 * Resolve the parent directory of a path and give the name of the last
 * component. The caller owns neither string: name points into buf.
 */
static struct ramfs_node *resolve_parent(const char *path, char *buf, k_size_t buf_size,
                                         char **name, int *err)
{
    k_size_t len;
    char *slash;
    struct ramfs_node *parent;

    if (err != NULL) {
        *err = KERR_INVAL;
    }

    if (path == NULL || path[0] != '/' || buf == NULL || name == NULL) {
        return NULL;
    }

    len = strlen(path);
    if (len + 1 > buf_size) {
        if (err != NULL) {
            *err = KERR_NAMETOOLONG;
        }
        return NULL;
    }

    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    slash = NULL;
    for (k_size_t i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '/') {
            slash = &buf[i];
        }
    }

    if (slash == buf) {
        *name = buf + 1;
        parent = g_root;
    } else {
        *slash = '\0';
        *name = slash + 1;
        parent = resolve_node(buf, err);
    }

    if (*name[0] == '\0') {
        if (err != NULL) {
            *err = KERR_INVAL;
        }
        return NULL;
    }

    if (parent == NULL) {
        return NULL;
    }

    if (parent->type != RAMFS_TYPE_DIR) {
        if (err != NULL) {
            *err = KERR_NOTDIR;
        }
        return NULL;
    }

    if (err != NULL) {
        *err = 0;
    }
    return parent;
}

static int ensure_block(struct ramfs_node *node, k_size_t index)
{
    k_u8_t **new_blocks;
    k_size_t new_cap;
    k_size_t i;

    if (index < node->block_count && node->blocks[index] != NULL) {
        return 0;
    }

    if (index >= node->block_cap) {
        new_cap = node->block_cap == 0 ? 4 : node->block_cap * 2;
        while (new_cap <= index) {
            new_cap *= 2;
        }

        if (!reserve_allows(new_cap * sizeof(*new_blocks))) {
            return KERR_NOSPC;
        }

        new_blocks = kmm_alloc(new_cap * sizeof(*new_blocks), KMM_PRIORITY_KERNEL);
        if (new_blocks == NULL) {
            return KERR_NOSPC;
        }

        for (i = 0; i < new_cap; i++) {
            new_blocks[i] = (i < node->block_count) ? node->blocks[i] : NULL;
        }
        kmm_free(node->blocks);
        node->blocks = new_blocks;
        node->block_cap = new_cap;
    }

    while (node->block_count <= index) {
        node->blocks[node->block_count] = NULL;
        node->block_count++;
    }

    if (node->blocks[index] == NULL) {
        k_u8_t *block;

        if (!reserve_allows(RAMFS_BLOCK_SIZE)) {
            return KERR_NOSPC;
        }

        block = kmm_alloc(RAMFS_BLOCK_SIZE, KMM_PRIORITY_KERNEL);
        if (block == NULL) {
            return KERR_NOSPC;
        }
        memset(block, 0, RAMFS_BLOCK_SIZE);
        node->blocks[index] = block;
    }

    return 0;
}

static void node_truncate_down(struct ramfs_node *node, k_u64_t size)
{
    k_size_t first = (k_size_t)((size + RAMFS_BLOCK_SIZE - 1u) / RAMFS_BLOCK_SIZE);
    k_size_t i;

    if (node == NULL || node->type != RAMFS_TYPE_FILE) {
        return;
    }

    for (i = first; i < node->block_count; i++) {
        if (node->blocks[i] != NULL) {
            block_free(node->blocks[i]);
            node->blocks[i] = NULL;
        }
    }

    if (first < node->block_count) {
        /* Clear tail bytes in the partially kept last block. */
        k_size_t tail = (k_size_t)(size % RAMFS_BLOCK_SIZE);
        if (tail != 0 && node->blocks[first - 1] != NULL) {
            memset(node->blocks[first - 1] + tail, 0, RAMFS_BLOCK_SIZE - tail);
        }
    }

    node->size = size;
    node->mtime = k_clock_now_unix();
    node->ctime = node->mtime;
}

static int ramfs_vfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct ramfs_node *node;
    struct ramfs_file *file;
    int err = 0;

    K_UNUSED(ctx);

    if (out == NULL || out->priv == NULL) {
        return KERR_INVAL;
    }

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);

    node = resolve_node(path, &err);

    if (node == NULL) {
        if ((flags & VFS_O_CREAT) == 0) {
            k_mutex_unlock(&g_lock);
            return err;
        }

        {
            char buf[VFS_PATH_MAX];
            char *name = NULL;
            struct ramfs_node *parent = resolve_parent(path, buf, sizeof(buf), &name, &err);
            struct ramfs_node *created;

            if (parent == NULL) {
                k_mutex_unlock(&g_lock);
                return err;
            }

            if ((flags & VFS_O_DIRECTORY) != 0) {
                k_mutex_unlock(&g_lock);
                return KERR_INVAL;
            }

            created = node_new(RAMFS_TYPE_FILE);
            if (created == NULL) {
                k_mutex_unlock(&g_lock);
                return KERR_NOSPC;
            }

            err = dir_add(parent, name, created);
            if (err != 0) {
                kmm_free(created);
                k_mutex_unlock(&g_lock);
                return err;
            }

            node = created;
        }
    } else {
        if ((flags & VFS_O_EXCL) != 0 && (flags & VFS_O_CREAT) != 0) {
            k_mutex_unlock(&g_lock);
            return KERR_EXIST;
        }

        if ((flags & VFS_O_DIRECTORY) != 0 && node->type != RAMFS_TYPE_DIR) {
            k_mutex_unlock(&g_lock);
            return KERR_NOTDIR;
        }

        if ((flags & VFS_O_TRUNC) != 0 && node->type == RAMFS_TYPE_FILE) {
            node_truncate_down(node, 0);
        }
    }

    if ((flags & VFS_O_DIRECTORY) != 0 && node->type != RAMFS_TYPE_DIR) {
        k_mutex_unlock(&g_lock);
        return KERR_NOTDIR;
    }

    file = (struct ramfs_file *)out->priv;
    memset(file, 0, sizeof(*file));
    file->node = node;
    file->append = ((flags & VFS_O_APPEND) != 0) ? 1 : 0;

    node->open_count++;
    out->inode = node->inode;
    out->size = node->size;
    out->offset = ((flags & VFS_O_APPEND) != 0 && node->type == RAMFS_TYPE_FILE) ? node->size : 0;

    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_read(void *ctx, struct vfs_file *vfs_file, void *buf, k_size_t size)
{
    struct ramfs_file *file;
    struct ramfs_node *node;
    k_u8_t *dst = (k_u8_t *)buf;
    k_u64_t offset;
    k_size_t done = 0;

    K_UNUSED(ctx);

    if (vfs_file == NULL || vfs_file->priv == NULL || buf == NULL) {
        return KERR_INVAL;
    }

    file = (struct ramfs_file *)vfs_file->priv;
    node = file->node;
    if (node == NULL || node->type != RAMFS_TYPE_FILE) {
        return KERR_ISDIR;
    }

    k_mutex_lock(&g_lock);

    offset = vfs_file->offset;
    while (done < size) {
        k_u64_t pos = offset + (k_u64_t)done;
        k_size_t block_index = (k_size_t)(pos / RAMFS_BLOCK_SIZE);
        k_size_t in_block = (k_size_t)(pos % RAMFS_BLOCK_SIZE);
        k_size_t chunk = RAMFS_BLOCK_SIZE - in_block;

        if (chunk > size - done) {
            chunk = size - done;
        }

        if (pos < node->size) {
            k_size_t avail = (k_size_t)(node->size - pos);
            if (chunk > avail) {
                chunk = avail;
            }
            if (block_index < node->block_count && node->blocks[block_index] != NULL) {
                memcpy(dst + done, node->blocks[block_index] + in_block, chunk);
            } else {
                memset(dst + done, 0, chunk);
            }
        } else {
            /* Reading past the end of the file answers zeros. */
            memset(dst + done, 0, chunk);
        }

        done += chunk;
    }

    vfs_file->offset = offset + (k_u64_t)done;
    vfs_file->size = node->size;

    k_mutex_unlock(&g_lock);
    return (int)done;
}

static int ramfs_vfs_write(void *ctx, struct vfs_file *vfs_file, const void *buf, k_size_t size)
{
    struct ramfs_file *file;
    struct ramfs_node *node;
    const k_u8_t *src = (const k_u8_t *)buf;
    k_u64_t offset;
    k_size_t done = 0;

    K_UNUSED(ctx);

    if (vfs_file == NULL || vfs_file->priv == NULL || buf == NULL) {
        return KERR_INVAL;
    }

    file = (struct ramfs_file *)vfs_file->priv;
    node = file->node;
    if (node == NULL || node->type != RAMFS_TYPE_FILE) {
        return KERR_ISDIR;
    }

    k_mutex_lock(&g_lock);

    offset = file->append != 0 ? node->size : vfs_file->offset;

    while (done < size) {
        k_u64_t pos = offset + (k_u64_t)done;
        k_size_t block_index = (k_size_t)(pos / RAMFS_BLOCK_SIZE);
        k_size_t in_block = (k_size_t)(pos % RAMFS_BLOCK_SIZE);
        k_size_t chunk = RAMFS_BLOCK_SIZE - in_block;
        int rc;

        if (chunk > size - done) {
            chunk = size - done;
        }

        rc = ensure_block(node, block_index);
        if (rc != 0) {
            if (done > 0) {
                break;
            }
            k_mutex_unlock(&g_lock);
            return rc;
        }

        memcpy(node->blocks[block_index] + in_block, src + done, chunk);
        done += chunk;

        if (pos + (k_u64_t)chunk > node->size) {
            node->size = pos + (k_u64_t)chunk;
        }
    }

    if (done > 0) {
        node->mtime = k_clock_now_unix();
        node->ctime = node->mtime;
    }

    vfs_file->offset = offset + (k_u64_t)done;
    vfs_file->size = node->size;

    k_mutex_unlock(&g_lock);
    return (int)done;
}

static int ramfs_vfs_truncate(void *ctx, struct vfs_file *vfs_file, k_u64_t size)
{
    struct ramfs_file *file;
    struct ramfs_node *node;

    K_UNUSED(ctx);

    if (vfs_file == NULL || vfs_file->priv == NULL) {
        return KERR_INVAL;
    }

    file = (struct ramfs_file *)vfs_file->priv;
    node = file->node;
    if (node == NULL || node->type != RAMFS_TYPE_FILE) {
        return KERR_ISDIR;
    }

    k_mutex_lock(&g_lock);

    if (size < node->size) {
        node_truncate_down(node, size);
    } else if (size > node->size) {
        /* Extending: blocks are allocated on write; reads return zeros. */
        if (size > (k_u64_t)0xFFFFFFFFu) {
            k_mutex_unlock(&g_lock);
            return KERR_TOOBIG;
        }
        node->size = size;
        node->mtime = k_clock_now_unix();
        node->ctime = node->mtime;
    }

    vfs_file->size = node->size;
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_close(void *ctx, struct vfs_file *vfs_file)
{
    struct ramfs_file *file;
    struct ramfs_node *node;

    K_UNUSED(ctx);

    if (vfs_file == NULL || vfs_file->priv == NULL) {
        return KERR_INVAL;
    }

    file = (struct ramfs_file *)vfs_file->priv;
    node = file->node;
    if (node == NULL) {
        return 0;
    }

    k_mutex_lock(&g_lock);
    if (node->open_count > 0) {
        node->open_count--;
    }
    vfs_file->size = node->size;
    node_release(node);
    file->node = NULL;
    k_mutex_unlock(&g_lock);
    return 0;
}

/*
 * The name of a directory entry in the buffer of the caller: at most
 * VFS_NAME_MAX bytes and always terminated. A name that is longer than the
 * field of `struct vfs_dirent` is cut here, rather than copied with `strncpy`
 * and a terminator written at the limit, which the compiler refuses to trust.
 */
static void ramfs_name_out(char *dst, const char *src)
{
    k_size_t i = 0;

    while (i < VFS_NAME_MAX && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static int ramfs_vfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                          k_size_t *count)
{
    struct ramfs_node *node;
    struct ramfs_entry *entry;
    k_size_t found = 0;
    int err = 0;

    K_UNUSED(ctx);

    if (out == NULL || count == NULL) {
        return KERR_INVAL;
    }

    *count = 0;

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);

    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    if (node->type != RAMFS_TYPE_DIR) {
        k_mutex_unlock(&g_lock);
        return KERR_NOTDIR;
    }

    for (entry = node->entries; entry != NULL && found < max; entry = entry->next) {
        struct ramfs_node *child = entry->node;
        struct vfs_dirent *dent = &out[found];

        memset(dent, 0, sizeof(*dent));
        ramfs_name_out(dent->name, entry->name);
        if (child != NULL) {
            dent->inode = child->inode;
            dent->type = child->type;
            dent->size = child->size;
        }
        found++;
    }

    *count = found;
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_readdir(void *ctx, struct vfs_file *vfs_file, struct vfs_dirent *out,
                             k_size_t max, k_size_t *count)
{
    struct ramfs_file *file;
    struct ramfs_node *node;
    struct ramfs_entry *entry;
    k_u64_t index = 0;
    k_size_t found = 0;

    K_UNUSED(ctx);

    if (vfs_file == NULL || vfs_file->priv == NULL || out == NULL || count == NULL) {
        return KERR_INVAL;
    }

    *count = 0;

    file = (struct ramfs_file *)vfs_file->priv;
    node = file->node;
    if (node == NULL || node->type != RAMFS_TYPE_DIR) {
        return KERR_NOTDIR;
    }

    k_mutex_lock(&g_lock);

    for (entry = node->entries; entry != NULL && found < max; entry = entry->next) {
        if (index < vfs_file->offset) {
            index++;
            continue;
        }

        {
            struct vfs_dirent *dent = &out[found];
            struct ramfs_node *child = entry->node;

            memset(dent, 0, sizeof(*dent));
            ramfs_name_out(dent->name, entry->name);
            if (child != NULL) {
                dent->inode = child->inode;
                dent->type = child->type;
                dent->size = child->size;
            }
        }

        found++;
        index++;
        vfs_file->offset = index;
    }

    *count = found;
    k_mutex_unlock(&g_lock);
    return 0;
}

static void fill_stat(const struct ramfs_node *node, struct vfs_stat *out)
{
    out->inode = node->inode;
    out->type = node->type;
    out->size = node->size;
    out->mode = node->mode;
    out->owner = node->owner;
    out->group = node->group;
}

static void fill_statx(const struct ramfs_node *node, struct vfs_statx *out)
{
    out->inode = node->inode;
    out->type = node->type;
    out->group = (k_u16_t)node->group;
    out->size = node->size;
    out->mode = node->mode;
    out->owner = node->owner;
    out->mtime = node->mtime;
    out->ctime = node->ctime;
}

static int ramfs_vfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    if (out == NULL) {
        return KERR_INVAL;
    }

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    memset(out, 0, sizeof(*out));
    fill_stat(node, out);
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_statx(void *ctx, const char *path, struct vfs_statx *out)
{
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    if (out == NULL) {
        return KERR_INVAL;
    }

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    memset(out, 0, sizeof(*out));
    fill_statx(node, out);
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_mkdir(void *ctx, const char *path)
{
    char buf[VFS_PATH_MAX];
    char *name = NULL;
    struct ramfs_node *parent;
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);

    if (resolve_node(path, &err) != NULL) {
        k_mutex_unlock(&g_lock);
        return KERR_EXIST;
    }

    parent = resolve_parent(path, buf, sizeof(buf), &name, &err);
    if (parent == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    node = node_new(RAMFS_TYPE_DIR);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return KERR_NOSPC;
    }

    err = dir_add(parent, name, node);
    if (err != 0) {
        kmm_free(node);
        k_mutex_unlock(&g_lock);
        return err;
    }

    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_unlink(void *ctx, const char *path)
{
    char buf[VFS_PATH_MAX];
    char *name = NULL;
    struct ramfs_node *parent;
    struct ramfs_node *target;
    struct ramfs_entry *entry;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);

    parent = resolve_parent(path, buf, sizeof(buf), &name, &err);
    if (parent == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    entry = dir_find(parent, name);
    if (entry == NULL) {
        k_mutex_unlock(&g_lock);
        return KERR_NOENT;
    }

    target = entry->node;
    if (target != NULL && target->type == RAMFS_TYPE_DIR) {
        k_mutex_unlock(&g_lock);
        return KERR_ISDIR;
    }

    err = dir_remove(parent, name, &target);
    if (err != 0) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    if (target != NULL) {
        target->ctime = k_clock_now_unix();
        node_release(target);
    }

    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_rmdir(void *ctx, const char *path)
{
    char buf[VFS_PATH_MAX];
    char *name = NULL;
    struct ramfs_node *parent;
    struct ramfs_node *target;
    struct ramfs_entry *entry;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);

    /* Do not remove the root of the ramfs itself. */
    if (strcmp(path, "/") == 0) {
        k_mutex_unlock(&g_lock);
        return KERR_BUSY;
    }

    parent = resolve_parent(path, buf, sizeof(buf), &name, &err);
    if (parent == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    entry = dir_find(parent, name);
    if (entry == NULL) {
        k_mutex_unlock(&g_lock);
        return KERR_NOENT;
    }

    target = entry->node;
    if (target == NULL || target->type != RAMFS_TYPE_DIR) {
        k_mutex_unlock(&g_lock);
        return KERR_NOTDIR;
    }

    if (!dir_is_empty(target)) {
        k_mutex_unlock(&g_lock);
        return KERR_NOTEMPTY;
    }

    err = dir_remove(parent, name, &target);
    if (err != 0) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    if (target != NULL) {
        target->ctime = k_clock_now_unix();
        node_release(target);
    }

    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_rename(void *ctx, const char *from, const char *to)
{
    char from_buf[VFS_PATH_MAX];
    char to_buf[VFS_PATH_MAX];
    char *from_name = NULL;
    char *to_name = NULL;
    struct ramfs_node *from_parent;
    struct ramfs_node *to_parent;
    struct ramfs_entry *from_entry;
    struct ramfs_entry *to_entry;
    struct ramfs_node *moved;
    struct ramfs_node *replaced = NULL;
    int err = 0;

    K_UNUSED(ctx);

    from = ramfs_relative(from);
    to = ramfs_relative(to);
    if (from == NULL || to == NULL) {
        return KERR_INVAL;
    }

    if (strcmp(from, "/") == 0) {
        return KERR_BUSY;
    }

    k_mutex_lock(&g_lock);

    from_parent = resolve_parent(from, from_buf, sizeof(from_buf), &from_name, &err);
    if (from_parent == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    to_parent = resolve_parent(to, to_buf, sizeof(to_buf), &to_name, &err);
    if (to_parent == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    from_entry = dir_find(from_parent, from_name);
    if (from_entry == NULL) {
        k_mutex_unlock(&g_lock);
        return KERR_NOENT;
    }

    moved = from_entry->node;

    if (strcmp(from_name, to_name) == 0 && from_parent == to_parent) {
        k_mutex_unlock(&g_lock);
        return 0;
    }

    to_entry = dir_find(to_parent, to_name);
    if (to_entry != NULL) {
        replaced = to_entry->node;
        if (replaced == moved) {
            k_mutex_unlock(&g_lock);
            return 0;
        }
        if (replaced != NULL && replaced->type == RAMFS_TYPE_DIR) {
            k_mutex_unlock(&g_lock);
            return KERR_ISDIR;
        }
        if (moved != NULL && moved->type == RAMFS_TYPE_DIR && replaced != NULL &&
            replaced->type != RAMFS_TYPE_DIR) {
            k_mutex_unlock(&g_lock);
            return KERR_NOTDIR;
        }

        err = dir_remove(to_parent, to_name, &replaced);
        if (err != 0) {
            k_mutex_unlock(&g_lock);
            return err;
        }
    }

    /* Remove from old parent. */
    {
        struct ramfs_entry *entry;
        struct ramfs_entry *prev = NULL;

        for (entry = from_parent->entries; entry != NULL; prev = entry, entry = entry->next) {
            if (entry == from_entry) {
                if (prev == NULL) {
                    from_parent->entries = entry->next;
                } else {
                    prev->next = entry->next;
                }
                break;
            }
        }

        /* Reuse the entry as the new name in the destination. */
        strncpy(from_entry->name, to_name, RAMFS_NAME_MAX);
        from_entry->name[RAMFS_NAME_MAX] = '\0';
        from_entry->next = to_parent->entries;
        to_parent->entries = from_entry;
    }

    if (from_parent != to_parent) {
        from_parent->mtime = k_clock_now_unix();
        from_parent->ctime = from_parent->mtime;
    }
    to_parent->mtime = k_clock_now_unix();
    to_parent->ctime = to_parent->mtime;
    if (moved != NULL) {
        moved->ctime = k_clock_now_unix();
    }

    if (replaced != NULL) {
        replaced->ctime = k_clock_now_unix();
        node_release(replaced);
    }

    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_chmod(void *ctx, const char *path, k_u32_t mode)
{
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    node->mode = mode & 07777u;
    node->ctime = k_clock_now_unix();
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_chown(void *ctx, const char *path, k_u32_t owner)
{
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    node->owner = owner;
    node->ctime = k_clock_now_unix();
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_chgrp(void *ctx, const char *path, k_u32_t group)
{
    struct ramfs_node *node;
    int err = 0;

    K_UNUSED(ctx);

    path = ramfs_relative(path);
    if (path == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    node = resolve_node(path, &err);
    if (node == NULL) {
        k_mutex_unlock(&g_lock);
        return err;
    }

    node->group = group;
    node->ctime = k_clock_now_unix();
    k_mutex_unlock(&g_lock);
    return 0;
}

static int ramfs_vfs_statfs(void *ctx, struct vfs_statfs *out)
{
    k_size_t free_heap;
    k_size_t total_heap;
    k_size_t reserve;
    k_size_t free_bytes;
    k_size_t total_bytes;

    K_UNUSED(ctx);

    if (out == NULL) {
        return KERR_INVAL;
    }

    memset(out, 0, sizeof(*out));

    free_heap = k_sysinfo_heap_free();
    total_heap = k_sysinfo_heap_total();
    reserve = RAMFS_RESERVE_BYTES;

    free_bytes = (free_heap > reserve) ? (free_heap - reserve) : 0;
    total_bytes = (total_heap > reserve) ? (total_heap - reserve) : 0;
    if (free_bytes > total_bytes) {
        free_bytes = total_bytes;
    }

    out->block_size = RAMFS_BLOCK_SIZE;
    out->total_blocks = (k_u64_t)(total_bytes / RAMFS_BLOCK_SIZE);
    out->free_blocks = (k_u64_t)(free_bytes / RAMFS_BLOCK_SIZE);
    out->total_inodes = 0;
    out->free_inodes = 0;

    return 0;
}

static const char *ramfs_vfs_strerror(int error)
{
    switch (error) {
    case KERR_NONE:
        return "no error";
    case KERR_NOENT:
        return "no such file or directory";
    case KERR_EXIST:
        return "file exists";
    case KERR_NOSPC:
        return "no space left on device";
    case KERR_INVAL:
        return "invalid argument";
    case KERR_NOTDIR:
        return "not a directory";
    case KERR_ISDIR:
        return "is a directory";
    case KERR_NOTEMPTY:
        return "directory not empty";
    case KERR_NOTSUP:
        return "operation not supported";
    case KERR_NAMETOOLONG:
        return "file name too long";
    case KERR_BUSY:
        return "device or resource busy";
    default:
        return "error";
    }
}

static const struct vfs_operations g_ramfs_ops = {
    .open = ramfs_vfs_open,
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .truncate = ramfs_vfs_truncate,
    .close = ramfs_vfs_close,
    .list = ramfs_vfs_list,
    .readdir = ramfs_vfs_readdir,
    .stat = ramfs_vfs_stat,
    .statx = ramfs_vfs_statx,
    .mkdir = ramfs_vfs_mkdir,
    .unlink = ramfs_vfs_unlink,
    .rmdir = ramfs_vfs_rmdir,
    .rename = ramfs_vfs_rename,
    .chmod = ramfs_vfs_chmod,
    .chown = ramfs_vfs_chown,
    .chgrp = ramfs_vfs_chgrp,
    .statfs = ramfs_vfs_statfs,
    .strerror = ramfs_vfs_strerror,
};

static void ramfs_free_tree(struct ramfs_node *node)
{
    struct ramfs_entry *entry;
    struct ramfs_entry *next;
    k_size_t i;

    if (node == NULL) {
        return;
    }

    if (node->type == RAMFS_TYPE_FILE) {
        for (i = 0; i < node->block_count; i++) {
            if (node->blocks[i] != NULL) {
                kmm_free(node->blocks[i]);
            }
        }
        kmm_free(node->blocks);
    } else {
        entry = node->entries;
        while (entry != NULL) {
            next = entry->next;
            ramfs_free_tree(entry->node);
            kmm_free(entry);
            entry = next;
        }
    }

    kmm_free(node);
}

int ramfs_vfs_reset(void)
{
    k_mutex_lock(&g_lock);
    if (g_root != NULL) {
        ramfs_free_tree(g_root);
        g_root = NULL;
    }
    g_mounted = false;
    g_next_inode = 1;
    g_point[0] = '\0';
    k_mutex_unlock(&g_lock);
    return 0;
}

int ramfs_vfs_register(const char *point)
{
    int rc;

    if (point == NULL) {
        return KERR_INVAL;
    }

    k_mutex_lock(&g_lock);
    if (g_root == NULL) {
        g_root = node_new(RAMFS_TYPE_DIR);
        if (g_root == NULL) {
            k_mutex_unlock(&g_lock);
            return KERR_NOSPC;
        }
        g_root->mode = 0777u;
        g_root->nlink = 1;
    }

    /* Remember the mount point for path stripping. */
    strncpy(g_point, point, sizeof(g_point) - 1);
    g_point[sizeof(g_point) - 1] = '\0';
    k_mutex_unlock(&g_lock);

    rc = vfs_mount(point, &g_ramfs_ops, NULL);
    if (rc != 0) {
        return rc;
    }

    k_mutex_lock(&g_lock);
    g_mounted = true;
    k_mutex_unlock(&g_lock);

    return 0;
}

static int ramfs_vfs_mount_type(const char *point, const char *device)
{
    K_UNUSED(device);
    return ramfs_vfs_register(point);
}

int ramfs_vfs_register_type(void)
{
    return vfs_register_type("ramfs", ramfs_vfs_mount_type);
}
