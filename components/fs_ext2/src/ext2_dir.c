/*
 * Micro OS — the directories of an ext2 volume: the entries of a variable
 * length, the names up to 255 bytes and the walk of a path.
 *
 * The name of a directory entry is compared byte for byte: ext2 is case
 * sensitive, and `README` and `readme` are two files. The listing copies a
 * name into the window of `struct vfs_dirent`, which is shorter, and cuts
 * it at a UTF-8 boundary; a path that names the file whole still reaches
 * it, because the lookup compares the name of the medium.
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/errno.h"

/* The fields of an entry that are always there, before the name. */
#define EXT2_DIRENT_HEADER 8u

/* The file type is a byte of the entry only when the volume says so. */
static bool has_file_type(const struct ext2_volume *vol)
{
    return (vol->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u;
}

int ext2_dir_next(struct ext2_volume *vol, const struct ext2_inode *dir, k_u64_t *pos,
                  struct ext2_dirent *out)
{
    k_u64_t size;

    if (vol == NULL || dir == NULL || pos == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (!ext2_inode_is_dir(dir)) {
        return KERR_NOTDIR;
    }

    size = dir->size;

    for (;;) {
        k_u8_t header[EXT2_DIRENT_HEADER];
        k_u64_t logical;
        k_u32_t inside;
        k_u32_t block;
        k_u64_t offset;
        k_u64_t block_end;
        k_u32_t inode;
        k_u16_t rec_len;
        k_size_t name_len;
        k_u8_t type;
        int rc;

        if (*pos >= size) {
            return 0;
        }

        if (*pos + EXT2_DIRENT_HEADER > size) {
            return KERR_IO;
        }

        /* The position is a byte of the directory stream; the block that
         * holds it comes out of the inode, as it does for a file. */
        logical = *pos / vol->block_size;
        inside = (k_u32_t)(*pos % vol->block_size);

        if (inside + EXT2_DIRENT_HEADER > vol->block_size) {
            return KERR_IO;
        }

        rc = ext2_block_for_logical(vol, dir, logical, &block);
        if (rc != 0) {
            return rc;
        }

        if (block == 0u) {
            return KERR_IO; /* a hole is not a directory */
        }

        offset = (k_u64_t)block * vol->block_size + inside;

        rc = ext2_read_bytes(vol, offset, header, sizeof(header));
        if (rc != 0) {
            return rc;
        }

        inode = ext2_le32(header + 0);
        rec_len = ext2_le16(header + 4);

        if (inode == 0u && rec_len == 0u) {
            return 0; /* the rest of the directory was never used */
        }

        if (rec_len < EXT2_DIRENT_HEADER || (rec_len & 3u) != 0u) {
            return KERR_IO;
        }

        /* An entry never crosses a block: the tail of a block belongs to
         * the last entry that starts in it. */
        block_end = (logical + 1u) * vol->block_size;
        if (*pos + rec_len > size || *pos + rec_len > block_end) {
            return KERR_IO;
        }

        if (inode == 0u) {
            *pos += rec_len;
            continue;
        }

        if (has_file_type(vol)) {
            name_len = header[6];
            type = header[7];
        } else {
            name_len = ext2_le16(header + 6);
            type = 0;
        }

        if (name_len == 0u || name_len > EXT2_NAME_MAX || name_len + EXT2_DIRENT_HEADER > rec_len) {
            return KERR_IO;
        }

        memset(out, 0, sizeof(*out));
        out->inode = inode;
        out->type = type;

        rc = ext2_read_bytes(vol, offset + EXT2_DIRENT_HEADER, out->name, name_len);
        if (rc != 0) {
            return rc;
        }
        out->name[name_len] = '\0';

        *pos += rec_len;

        return 1;
    }
}

static int find_in_dir(struct ext2_volume *vol, const struct ext2_inode *dir, const char *name,
                       k_u32_t *inode)
{
    struct ext2_dirent entry;
    k_u64_t pos = 0;
    int rc;

    for (;;) {
        rc = ext2_dir_next(vol, dir, &pos, &entry);
        if (rc < 0) {
            return rc;
        }

        if (rc == 0) {
            return KERR_NOENT;
        }

        if (strcmp(entry.name, name) == 0) {
            *inode = entry.inode;
            return 0;
        }
    }
}

int ext2_lookup(struct ext2_volume *vol, const char *path, struct ext2_inode *out, k_u32_t *number)
{
    const char *p = path;
    struct ext2_inode current;
    k_u32_t inode = EXT2_ROOT_INODE;
    int rc;

    if (vol == NULL || path == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    while (*p == '/') {
        p++;
    }

    rc = ext2_read_inode(vol, inode, &current);
    if (rc != 0) {
        return rc;
    }

    if (*p == '\0') {
        *out = current;
        if (number != NULL) {
            *number = inode;
        }
        return 0;
    }

    for (;;) {
        char component[EXT2_NAME_MAX + 1];
        k_size_t len = 0;
        k_u32_t found;

        while (*p != '\0' && *p != '/') {
            if (len >= EXT2_NAME_MAX) {
                return KERR_NAMETOOLONG;
            }
            component[len++] = *p++;
        }
        component[len] = '\0';

        if (!ext2_inode_is_dir(&current)) {
            return KERR_NOTDIR;
        }

        rc = find_in_dir(vol, &current, component, &found);
        if (rc != 0) {
            return rc;
        }

        rc = ext2_read_inode(vol, found, &current);
        if (rc != 0) {
            return rc;
        }

        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            *out = current;
            if (number != NULL) {
                *number = found;
            }
            return 0;
        }
    }
}
