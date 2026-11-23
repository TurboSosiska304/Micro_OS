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

int ext2_dir_find(struct ext2_volume *vol, const struct ext2_inode *dir, const char *name,
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

/* The shortest record a name of this length occupies: the header, the name
 * and the padding to a word. */
static k_u16_t entry_length(k_size_t name_len)
{
    return (k_u16_t)((EXT2_DIRENT_HEADER + name_len + 3u) & ~3u);
}

static void entry_write(k_u8_t *p, k_u32_t inode, const char *name, k_u8_t type, bool file_type,
                        k_u16_t rec_len)
{
    k_size_t len = strlen(name);

    ext2_put32(p + 0, inode);
    ext2_put16(p + 4, rec_len);

    if (file_type) {
        p[6] = (k_u8_t)len;
        p[7] = type;
    } else {
        ext2_put16(p + 6, (k_u16_t)len);
        p[7] = 0;
    }

    memcpy(p + EXT2_DIRENT_HEADER, name, len);
}

/*
 * Add a record to a directory. A hole of the records that are gone takes the
 * entry when it is large enough; an entry with room to spare is cut in two,
 * the new record at its tail; and a directory with no room in its blocks
 * grows by one block, which is a record covering the whole block.
 */
int ext2_dir_insert(struct ext2_volume *vol, struct ext2_inode *dir, const char *name,
                    k_u32_t inode, k_u8_t type)
{
    k_size_t name_len;
    k_u16_t ideal;
    k_u64_t blocks;
    k_u64_t logical;

    if (vol == NULL || dir == NULL || name == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (!ext2_inode_is_dir(dir) || inode == 0) {
        return KERR_INVAL;
    }

    name_len = strlen(name);
    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        return KERR_NAMETOOLONG;
    }

    if ((dir->size % vol->block_size) != 0u) {
        return KERR_IO; /* a directory is a whole number of blocks */
    }

    ideal = entry_length(name_len);
    blocks = (dir->size + vol->block_size - 1u) / vol->block_size;

    for (logical = 0; logical < blocks; logical++) {
        k_u32_t block = 0;
        k_u32_t offset = 0;
        int rc;

        rc = ext2_inode_get_block(vol, dir, logical, &block, false, NULL);
        if (rc != 0) {
            return rc;
        }

        if (block == 0u) {
            return KERR_IO; /* a directory has no holes */
        }

        rc = ext2_read_block(vol, block, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        while (offset + EXT2_DIRENT_HEADER <= vol->block_size) {
            k_u8_t *p = vol->block_buf + offset;
            k_u32_t used = ext2_le32(p + 0);
            k_u16_t rec_len = ext2_le16(p + 4);

            if (rec_len < EXT2_DIRENT_HEADER || (rec_len & 3u) != 0u ||
                offset + rec_len > vol->block_size) {
                return KERR_IO;
            }

            if (used == 0u) {
                if (rec_len >= ideal) {
                    entry_write(p, inode, name, type, has_file_type(vol), rec_len);
                    return ext2_write_block(vol, block, vol->block_buf);
                }
            } else {
                k_size_t used_len;
                k_u16_t shortest;

                if (has_file_type(vol)) {
                    used_len = p[6];
                } else {
                    used_len = ext2_le16(p + 6);
                }

                if (used_len == 0u || used_len + EXT2_DIRENT_HEADER > rec_len) {
                    return KERR_IO; /* a record whose name does not fit it */
                }

                shortest = entry_length(used_len);

                if (shortest <= rec_len && rec_len >= (k_u16_t)(shortest + ideal)) {
                    /* The record of the entry that is there shrinks and the
                     * new one takes the tail of its room. */
                    ext2_put16(p + 4, (k_u16_t)(rec_len - ideal));
                    entry_write(p + rec_len - ideal, inode, name, type, has_file_type(vol), ideal);
                    return ext2_write_block(vol, block, vol->block_buf);
                }
            }

            offset += rec_len;
        }
    }

    /* No block has room: the directory grows by one, and the new block is
     * one record with all of its room for the name. */
    logical = blocks;
    {
        k_u32_t block = 0;
        bool fresh = false;
        int rc;

        rc = ext2_inode_get_block(vol, dir, logical, &block, true, &fresh);
        if (rc != 0) {
            return rc;
        }

        if (block == 0u) {
            return KERR_IO;
        }

        /* The bytes of a block this call allocated are nobody's: the record
         * of the name covers the whole block and the rest of it is zeros,
         * which a reader of the directory never sees. */
        if (fresh) {
            memset(vol->block_buf, 0, vol->block_size);

            /* The bit of the block reaches the medium before the block. */
            rc = ext2_write_flush(vol);
            if (rc != 0) {
                return rc;
            }
        }

        entry_write(vol->block_buf, inode, name, type, has_file_type(vol),
                    (k_u16_t)vol->block_size);
        dir->size = (blocks + 1u) * vol->block_size;

        return ext2_write_block(vol, block, vol->block_buf);
    }
}

/*
 * Take a record out of a directory: the room goes to the record in front of
 * it, so the stream of the block stays one chain from the first byte to the
 * last. A record that begins a block has no record in front of it and is
 * marked free in place.
 */
int ext2_dir_remove(struct ext2_volume *vol, struct ext2_inode *dir, const char *name)
{
    k_u64_t blocks;

    if (vol == NULL || dir == NULL || name == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (!ext2_inode_is_dir(dir)) {
        return KERR_NOTDIR;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return KERR_INVAL;
    }

    blocks = (dir->size + vol->block_size - 1u) / vol->block_size;

    for (k_u64_t logical = 0; logical < blocks; logical++) {
        k_u32_t block = 0;
        k_u32_t offset = 0;
        k_u32_t previous = 0;
        bool has_previous = false;
        int rc;

        rc = ext2_inode_get_block(vol, dir, logical, &block, false, NULL);
        if (rc != 0) {
            return rc;
        }

        if (block == 0u) {
            return KERR_IO;
        }

        rc = ext2_read_block(vol, block, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        while (offset + EXT2_DIRENT_HEADER <= vol->block_size) {
            k_u8_t *p = vol->block_buf + offset;
            k_u32_t used = ext2_le32(p + 0);
            k_u16_t rec_len = ext2_le16(p + 4);

            if (rec_len < EXT2_DIRENT_HEADER || (rec_len & 3u) != 0u ||
                offset + rec_len > vol->block_size) {
                return KERR_IO;
            }

            if (used != 0u) {
                k_size_t name_len = has_file_type(vol) ? p[6] : ext2_le16(p + 6);

                if (name_len == 0u || name_len > EXT2_NAME_MAX ||
                    name_len + EXT2_DIRENT_HEADER > rec_len) {
                    return KERR_IO;
                }

                if (memcmp(p + EXT2_DIRENT_HEADER, name, name_len) == 0 && name[name_len] == '\0') {
                    if (has_previous) {
                        k_u8_t *q = vol->block_buf + previous;
                        ext2_put16(q + 4, (k_u16_t)(ext2_le16(q + 4) + rec_len));
                    } else {
                        ext2_put32(p + 0, 0);
                    }

                    return ext2_write_block(vol, block, vol->block_buf);
                }
            }

            previous = offset;
            has_previous = true;
            offset += rec_len;
        }
    }

    return KERR_NOENT;
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

        rc = ext2_dir_find(vol, &current, component, &found);
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
