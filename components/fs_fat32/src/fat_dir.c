/*
 * Micro OS — the directories of a FAT volume: the entries of 32 bytes, the
 * long names of the world beyond 8.3 and the lookup that ignores case.
 *
 * A long name is kept in UTF-16 on the medium and the shell reads UTF-8, so
 * the scanner translates it. The name of an entry of the VFS is 32 bytes by
 * contract: a listing truncates a longer name (a path that names it is a
 * different call, and it is compared whole).
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/errno.h"

/* One unit of the name area of a long entry: 13 UTF-16 code units. */
#define LFN_UNITS_PER_ENTRY 13u
#define LFN_CHARS_1 5u
#define LFN_CHARS_2 6u
#define LFN_CHARS_3 2u

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool name_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower_ascii(*a) != lower_ascii(*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == *b;
}

static k_u8_t short_checksum(const k_u8_t *name)
{
    k_u8_t sum = 0;
    k_size_t i;

    for (i = 0; i < 11; i++) {
        sum = (k_u8_t)(((sum & 1u) << 7) + (sum >> 1) + name[i]);
    }

    return sum;
}

/*
 * UTF-16 to UTF-8. A character that does not fit is not written at all:
 * a truncated multibyte sequence would be a different name, and a caller
 * that compares it would be told a lie about the volume.
 */
static k_size_t utf16_to_utf8(const k_u16_t *units, k_size_t count, char *out, k_size_t cap)
{
    k_size_t used = 0;
    k_size_t i = 0;

    while (i < count) {
        k_u32_t cp = units[i++];

        if (cp == 0x0000 || cp == 0xFFFFu) {
            break;
        }

        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            k_u32_t low;

            if (i >= count) {
                break;
            }
            low = units[i];
            if (low < 0xDC00u || low > 0xDFFFu) {
                break;
            }
            i++;
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            break;
        }

        if (cp < 0x80u) {
            if (used + 1 >= cap) {
                break;
            }
            out[used++] = (char)cp;
        } else if (cp < 0x800u) {
            if (used + 2 >= cap) {
                break;
            }
            out[used++] = (char)(0xC0u | (cp >> 6));
            out[used++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (used + 3 >= cap) {
                break;
            }
            out[used++] = (char)(0xE0u | (cp >> 12));
            out[used++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[used++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (used + 4 >= cap) {
                break;
            }
            out[used++] = (char)(0xF0u | (cp >> 18));
            out[used++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[used++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[used++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }

    out[used] = '\0';
    return used;
}

static void lfn_reset(struct fat_volume *vol)
{
    vol->lfn_active = false;
    vol->lfn_count = 0;
    vol->lfn_next = 0;
    vol->lfn_checksum = 0;
}

static void lfn_store(struct fat_volume *vol, k_u8_t sequence, const k_u8_t *entry)
{
    k_size_t base = (k_size_t)(sequence - 1u) * LFN_UNITS_PER_ENTRY;
    k_size_t i;

    for (i = 0; i < LFN_CHARS_1; i++) {
        vol->lfn[base + i] = (k_u16_t)(entry[1 + i * 2] | (entry[2 + i * 2] << 8));
    }
    for (i = 0; i < LFN_CHARS_2; i++) {
        vol->lfn[base + LFN_CHARS_1 + i] = (k_u16_t)(entry[14 + i * 2] | (entry[15 + i * 2] << 8));
    }
    for (i = 0; i < LFN_CHARS_3; i++) {
        vol->lfn[base + LFN_CHARS_1 + LFN_CHARS_2 + i] =
            (k_u16_t)(entry[28 + i * 2] | (entry[29 + i * 2] << 8));
    }
}

/*
 * The long name of the entries that were collected, when the run is whole
 * and belongs to this short entry. Returns false for a run that a crash or
 * another writer left broken; the short name is then the name.
 */
static bool lfn_to_name(struct fat_volume *vol, const k_u8_t *short_entry, char *out, k_size_t cap)
{
    k_u8_t checksum = short_checksum(short_entry);
    k_size_t units;

    if (!vol->lfn_active || vol->lfn_count == 0 || vol->lfn_next != 0 ||
        vol->lfn_checksum != checksum) {
        return false;
    }

    units = (k_size_t)vol->lfn_count * LFN_UNITS_PER_ENTRY;
    if (units > FAT_LFN_UNITS) {
        units = FAT_LFN_UNITS;
    }

    return utf16_to_utf8(vol->lfn, units, out, cap) > 0;
}

/* The 8.3 name as a person reads it in a listing. */
static void short_name(const k_u8_t *entry, char *out, k_size_t cap)
{
    char base[9];
    char ext[4];
    k_size_t blen = 8;
    k_size_t elen = 3;
    k_size_t used = 0;
    k_size_t i;

    memcpy(base, entry, 8);
    memcpy(ext, entry + 8, 3);

    while (blen > 0 && base[blen - 1] == ' ') {
        blen--;
    }
    while (elen > 0 && ext[elen - 1] == ' ') {
        elen--;
    }

    if (entry[0] == 0x05) {
        base[0] = (char)0xE5;
    }

    for (i = 0; i < blen && used + 1 < cap; i++) {
        char c = base[i];
        if ((entry[12] & 0x08u) != 0u) {
            c = lower_ascii(c);
        }
        out[used++] = c;
    }

    if (elen > 0 && used + 1 < cap) {
        out[used++] = '.';
    }

    for (i = 0; i < elen && used + 1 < cap; i++) {
        char c = ext[i];
        if ((entry[12] & 0x10u) != 0u) {
            c = lower_ascii(c);
        }
        out[used++] = c;
    }

    out[used] = '\0';
}

int fat_scan_next(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t *pos, struct fat_entry *out)
{
    k_u8_t raw[32];
    int rc;

    if (vol == NULL || pos == NULL || out == NULL) {
        return KERR_INVAL;
    }

    for (;;) {
        if (dir_cluster == 0) {
            if (*pos >= vol->root_entries * 32u) {
                return 0;
            }
            rc = fat_read_volume(vol, (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + *pos,
                                 raw, sizeof(raw));
            if (rc != 0) {
                return rc;
            }
        } else {
            k_size_t got = 0;

            rc = fat_read_chain(vol, dir_cluster, *pos, raw, sizeof(raw), &got);
            if (rc != 0) {
                return rc;
            }
            if (got < sizeof(raw)) {
                return 0;
            }
        }

        *pos += sizeof(raw);

        if (raw[0] == 0x00) {
            return 0; /* the end of the directory */
        }

        if (raw[0] == 0xE5) {
            lfn_reset(vol); /* a removed entry takes its long name with it */
            continue;
        }

        if (raw[11] == FAT_ATTR_LFN) {
            k_u8_t sequence = (k_u8_t)(raw[0] & 0x1Fu);
            bool last = (raw[0] & 0x40u) != 0u;

            if (sequence == 0 || sequence > FAT_LFN_ENTRIES) {
                lfn_reset(vol);
                continue;
            }

            if (last) {
                lfn_reset(vol);
                vol->lfn_checksum = raw[13];
                vol->lfn_count = sequence;
                vol->lfn_next = (k_u8_t)(sequence - 1u);
                vol->lfn_active = true;
                lfn_store(vol, sequence, raw);
            } else if (vol->lfn_active && sequence == vol->lfn_next) {
                lfn_store(vol, sequence, raw);
                vol->lfn_next = (k_u8_t)(sequence - 1u);
            } else {
                lfn_reset(vol);
            }
            continue;
        }

        if ((raw[11] & FAT_ATTR_VOLUME) != 0u || raw[0] == '.') {
            lfn_reset(vol);
            continue;
        }

        memset(out, 0, sizeof(*out));

        if (!lfn_to_name(vol, raw, out->name, sizeof(out->name))) {
            short_name(raw, out->name, sizeof(out->name));
        }

        if (out->name[0] == '\0') {
            lfn_reset(vol);
            continue;
        }

        out->attr = raw[11];
        out->dir = (raw[11] & FAT_ATTR_DIR) != 0u;
        out->cluster = (k_u32_t)fat_le16(raw + 26) | ((k_u32_t)fat_le16(raw + 20) << 16);
        out->size = fat_le32(raw + 28);

        lfn_reset(vol);

        return 1;
    }
}

static int find_in_dir(struct fat_volume *vol, k_u32_t dir_cluster, const char *name,
                       struct fat_entry *out)
{
    k_u32_t pos = 0;
    int rc;

    lfn_reset(vol);

    for (;;) {
        rc = fat_scan_next(vol, dir_cluster, &pos, out);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return KERR_NOENT;
        }
        if (name_equal(out->name, name)) {
            return 0;
        }
    }
}

int fat_lookup(struct fat_volume *vol, const char *path, struct fat_entry *out)
{
    const char *p = path;
    k_u32_t cluster;
    struct fat_entry entry;
    int rc;

    if (vol == NULL || path == NULL || out == NULL) {
        return KERR_INVAL;
    }

    while (*p == '/') {
        p++;
    }

    if (*p == '\0') {
        memset(out, 0, sizeof(*out));
        out->dir = true;
        out->attr = FAT_ATTR_DIR;
        out->cluster = (vol->type == 32) ? vol->root_cluster : 0;
        out->name[0] = '/';
        out->name[1] = '\0';
        return 0;
    }

    cluster = (vol->type == 32) ? vol->root_cluster : 0;

    for (;;) {
        char component[FAT_NAME_MAX + 1];
        k_size_t len = 0;

        while (*p != '\0' && *p != '/') {
            if (len >= FAT_NAME_MAX) {
                return KERR_NOENT;
            }
            component[len++] = *p++;
        }
        component[len] = '\0';

        if (len == 0) {
            return KERR_NOENT;
        }

        rc = find_in_dir(vol, cluster, component, &entry);
        if (rc != 0) {
            return rc;
        }

        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            *out = entry;
            return 0;
        }

        if (!entry.dir) {
            return KERR_NOTDIR;
        }
        if (entry.cluster < 2u) {
            return KERR_IO;
        }

        cluster = entry.cluster;
    }
}
