/*
 * Micro OS — the directories of a FAT volume: the entries of 32 bytes, the
 * long names of the world beyond 8.3 and the lookup that ignores case.
 *
 * A long name is kept in UTF-16 on the medium and the shell reads UTF-8, so
 * the scanner translates it. The name of an entry of the VFS is 32 bytes by
 * contract: a listing truncates a longer name (a path that names it is a
 * different call, and it is compared whole).
 *
 * The same file creates what a name needs: a run of long-name entries with
 * the checksum of the short name and a generated 8.3 entry in front of it,
 * and it marks both of them removed when the object goes. The fixed root of
 * FAT12/16 cannot grow; a root or a subdirectory of clusters does.
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/clock.h"
#include "kernel/errno.h"

/* One unit of the name area of a long entry: 13 UTF-16 code units. */
#define LFN_UNITS_PER_ENTRY 13u
#define LFN_CHARS_1 5u
#define LFN_CHARS_2 6u
#define LFN_CHARS_3 2u

/*
 * The window of a date of FAT. The format starts at 1980 and ends at 2107,
 * and the machine of this system has no wall clock of its own: the stamp is
 * the seconds since the boot, shifted to the first moment the medium can
 * hold. A volume written by another machine keeps its own dates, and the
 * number comes back the way that machine wrote it.
 */
#define FAT_UNIX_1980 315532800ull
#define FAT_UNIX_MAX 4354819198ull

static bool is_leap_year(k_u32_t year)
{
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static k_u32_t month_days(k_u32_t year, k_u32_t month)
{
    static const k_u8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2u && is_leap_year(year)) {
        return 29;
    }

    return days[month - 1u];
}

/* The seconds of Unix of the date and the time of an entry; 0 when the
 * fields are not a date, which is how an old image says it kept none. */
static k_u64_t fat_time_decode(k_u16_t date, k_u16_t time)
{
    k_u32_t year = 1980u + (date >> 9);
    k_u32_t month = (date >> 5) & 0x0Fu;
    k_u32_t day = date & 0x1Fu;
    k_u32_t hour = (time >> 11) & 0x1Fu;
    k_u32_t minute = (time >> 5) & 0x3Fu;
    k_u32_t second = (time & 0x1Fu) * 2u;
    k_u64_t days = 0;
    k_u32_t i;

    if (date == 0 || month < 1u || month > 12u || day < 1u || day > month_days(year, month)) {
        return 0;
    }
    if (hour > 23u || minute > 59u || second > 59u) {
        return 0;
    }

    for (i = 1970u; i < year; i++) {
        days += is_leap_year(i) ? 366u : 365u;
    }
    for (i = 1u; i < month; i++) {
        days += month_days(year, i);
    }

    return (days + day - 1u) * 86400u + hour * 3600u + minute * 60u + second;
}

/* The two words of an entry for a moment of Unix, in the window of FAT. */
static void fat_time_encode(k_u64_t when, k_u16_t *date, k_u16_t *time)
{
    k_u32_t days;
    k_u32_t seconds;
    k_u32_t year = 1970u;
    k_u32_t month = 1u;
    k_u32_t day;

    if (when < FAT_UNIX_1980) {
        when = FAT_UNIX_1980;
    }
    if (when > FAT_UNIX_MAX) {
        when = FAT_UNIX_MAX;
    }

    days = (k_u32_t)(when / 86400u);
    seconds = (k_u32_t)(when % 86400u);

    while (year < 2108u) {
        k_u32_t span = is_leap_year(year) ? 366u : 365u;

        if (days < span) {
            break;
        }
        days -= span;
        year++;
    }

    while (month < 12u) {
        k_u32_t span = month_days(year, month);

        if (days < span) {
            break;
        }
        days -= span;
        month++;
    }

    day = days + 1u;

    *date = (k_u16_t)(((year - 1980u) << 9) | (month << 5) | day);
    *time = (k_u16_t)(((seconds / 3600u) << 11) | (((seconds / 60u) % 60u) << 5) |
                      ((seconds % 60u) / 2u));
}

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
        out->mtime = fat_time_decode(fat_le16(raw + 24), fat_le16(raw + 22));

        lfn_reset(vol);

        return 1;
    }
}

static int find_in_dir(struct fat_volume *vol, k_u32_t dir_cluster, const char *name,
                       struct fat_entry *out, k_u32_t *pos_out)
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
            if (pos_out != NULL) {
                *pos_out = pos - 32u;
            }
            return 0;
        }
    }
}

int fat_lookup_pos(struct fat_volume *vol, const char *path, struct fat_entry *out,
                   k_u32_t *dir_cluster, k_u32_t *pos)
{
    const char *p = path;
    k_u32_t cluster;
    struct fat_entry entry;
    int rc;

    if (vol == NULL || path == NULL || out == NULL) {
        return KERR_INVAL;
    }

    if (dir_cluster != NULL) {
        *dir_cluster = FAT_POS_NONE;
    }
    if (pos != NULL) {
        *pos = FAT_POS_NONE;
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
        k_u32_t entry_pos = FAT_POS_NONE;

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

        rc = find_in_dir(vol, cluster, component, &entry, &entry_pos);
        if (rc != 0) {
            return rc;
        }

        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            *out = entry;
            if (dir_cluster != NULL) {
                *dir_cluster = cluster;
            }
            if (pos != NULL) {
                *pos = entry_pos;
            }
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

int fat_lookup(struct fat_volume *vol, const char *path, struct fat_entry *out)
{
    return fat_lookup_pos(vol, path, out, NULL, NULL);
}

int fat_lookup_parent(struct fat_volume *vol, const char *path, struct fat_entry *parent,
                      char *leaf, k_size_t cap)
{
    char buffer[VFS_PATH_MAX];
    k_size_t len;
    k_size_t slash;
    k_size_t leaf_len;

    if (vol == NULL || path == NULL || parent == NULL || leaf == NULL || cap == 0) {
        return KERR_INVAL;
    }

    len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        len--;
    }

    if (len == 0) {
        return KERR_INVAL;
    }

    slash = len;
    while (slash > 0 && path[slash - 1] != '/') {
        slash--;
    }

    leaf_len = len - slash;
    if (leaf_len == 0) {
        return KERR_INVAL;
    }
    if (leaf_len > FAT_NAME_MAX || leaf_len >= cap) {
        return KERR_NAMETOOLONG;
    }

    memcpy(leaf, path + slash, leaf_len);
    leaf[leaf_len] = '\0';

    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        return KERR_INVAL;
    }

    if (slash <= 1) {
        buffer[0] = '/';
        buffer[1] = '\0';
    } else {
        if (slash >= sizeof(buffer)) {
            return KERR_NAMETOOLONG;
        }
        memcpy(buffer, path, slash);
        buffer[slash] = '\0';
    }

    return fat_lookup(vol, buffer, parent);
}

/* ------------------------------------------------------------------------
 * The writer of the entries
 *
 * A directory is a stream of 32-byte entries; the fixed root of FAT12/16 is
 * the only one that cannot grow. The calls below keep the position of an
 * entry a byte offset into that stream, the same number `fat_scan_next`
 * speaks, so a removal marks the entries the scanner returned.
 * --------------------------------------------------------------------- */

static int dir_read_raw(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t pos, k_u8_t raw[32])
{
    if (dir_cluster == 0) {
        if (pos >= vol->root_entries * 32u) {
            return KERR_NOENT;
        }
        return fat_read_volume(vol, (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + pos, raw,
                               32);
    }

    {
        k_size_t got = 0;
        int rc = fat_read_chain(vol, dir_cluster, pos, raw, 32, &got);

        if (rc != 0) {
            return rc;
        }
        return (got == 32) ? 0 : KERR_NOENT;
    }
}

static int dir_write_raw(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t pos,
                         const k_u8_t raw[32])
{
    if (dir_cluster == 0) {
        if (pos + 32u > vol->root_entries * 32u) {
            return KERR_NOSPC;
        }
        return fat_write_volume(vol, (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + pos,
                                raw, 32);
    }

    {
        k_size_t done = 0;
        int rc = fat_write_chain(vol, dir_cluster, pos, raw, 32, &done);

        if (rc != 0) {
            return rc;
        }
        return (done == 32) ? 0 : KERR_IO;
    }
}

/* The bytes of a directory that already exist, fixed root or chain. */
static int dir_bytes(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t *out)
{
    k_u32_t clusters;

    if (dir_cluster == 0) {
        *out = vol->root_entries * 32u;
        return 0;
    }

    {
        int rc = fat_chain_count(vol, dir_cluster, &clusters);

        if (rc != 0) {
            return rc;
        }
    }

    *out = clusters * vol->cluster_bytes;
    return 0;
}

/*
 * The first place of `count` free slots in a row. A long name needs such a
 * run: its entries have to sit immediately before the short entry. A
 * directory of clusters grows by a cluster at a time; the fixed root of the
 * old cards does not grow and says KERR_NOSPC when it is full.
 */
static int dir_find_slots(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t count, k_u32_t *out)
{
    k_u32_t bytes;
    k_u32_t pos = 0;
    k_u32_t run = 0;
    k_u32_t start = 0;
    k_u8_t raw[32];
    int rc;

    if (count == 0) {
        *out = 0;
        return 0;
    }

    rc = dir_bytes(vol, dir_cluster, &bytes);
    if (rc != 0) {
        return rc;
    }

    while (pos + 32u <= bytes) {
        rc = dir_read_raw(vol, dir_cluster, pos, raw);
        if (rc != 0) {
            return rc;
        }

        if (raw[0] == 0x00 || raw[0] == 0xE5) {
            if (run == 0) {
                start = pos;
            }
            run++;
            if (run >= count) {
                *out = start;
                return 0;
            }
        } else {
            run = 0;
        }

        pos += 32u;
    }

    if (dir_cluster == 0) {
        return KERR_NOSPC;
    }

    while (run < count) {
        k_u32_t first = dir_cluster;
        k_u32_t current;

        rc = fat_chain_count(vol, dir_cluster, &current);
        if (rc != 0) {
            return rc;
        }

        rc = fat_chain_extend(vol, &first, current + 1u);
        if (rc != 0) {
            return rc;
        }

        if (run == 0) {
            start = bytes;
        }
        bytes += vol->cluster_bytes;
        run += vol->cluster_bytes / 32u;
    }

    *out = start;
    return 0;
}

static bool plain_short_char(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }

    return (c != '\0') && (strchr("$%'-_@~`!(){}^#&", c) != NULL);
}

/*
 * The 8.3 name of a long one: upper case, at most 8 and 3 characters, the
 * characters FAT allows in a short name. `plain` says that the name already
 * was such a name and the entry needs no run of long-name entries.
 */
static void make_short_name(const char *name, k_u8_t out[11], bool *plain)
{
    const char *dot = strrchr(name, '.');
    const char *base_start = name;
    const char *base_end;
    const char *p;
    k_size_t blen = 0;
    k_size_t elen = 0;
    bool simple = true;

    memset(out, ' ', 11);

    if (dot == name) {
        dot = NULL; /* a leading dot belongs to the long name only */
    }

    while (*base_start == '.') {
        base_start++;
        simple = false;
    }

    base_end = (dot != NULL) ? dot : name + strlen(name);
    if (base_end < base_start) {
        base_end = base_start;
    }

    for (p = base_start; p < base_end; p++) {
        char c = *p;

        if (c == ' ' || c == '.') {
            simple = false;
            continue;
        }

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
            simple = false;
        }

        if (!plain_short_char(c)) {
            c = '_';
            simple = false;
        }

        if (blen >= 8u) {
            simple = false;
            continue;
        }

        out[blen++] = (k_u8_t)c;
    }

    if (blen == 0) {
        out[0] = (k_u8_t)'_';
        simple = false;
    }

    if (dot != NULL) {
        const char *ext = dot + 1;

        if (*ext == '\0') {
            simple = false; /* a trailing dot is a part of the long name */
        }

        for (p = ext; *p != '\0'; p++) {
            char c = *p;

            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
                simple = false;
            }

            if (!plain_short_char(c)) {
                c = '_';
                simple = false;
            }

            if (elen >= 3u) {
                simple = false;
                continue;
            }

            out[8 + elen++] = (k_u8_t)c;
        }
    }

    *plain = simple;
}

static int short_exists(struct fat_volume *vol, k_u32_t dir_cluster, const k_u8_t name11[11],
                        bool *found)
{
    k_u32_t bytes;
    k_u32_t pos = 0;
    k_u8_t raw[32];
    int rc;

    *found = false;

    rc = dir_bytes(vol, dir_cluster, &bytes);
    if (rc != 0) {
        return rc;
    }

    while (pos + 32u <= bytes) {
        rc = dir_read_raw(vol, dir_cluster, pos, raw);
        if (rc != 0) {
            return rc;
        }
        if (raw[0] == 0x00) {
            return 0;
        }
        if (raw[0] != 0xE5 && raw[11] != FAT_ATTR_LFN && memcmp(raw, name11, 11) == 0) {
            *found = true;
            return 0;
        }
        pos += 32u;
    }

    return 0;
}

/* "BASE~N.EXT", the name FAT gives to the second file of a name. */
static void short_variant(const k_u8_t name11[11], k_u32_t number, k_u8_t out[11])
{
    k_size_t blen = 8;
    k_size_t elen = 3;
    k_size_t digits = 1;
    k_size_t keep;
    k_u32_t rest = number;

    while (blen > 0 && name11[blen - 1] == ' ') {
        blen--;
    }
    while (elen > 0 && name11[8 + elen - 1] == ' ') {
        elen--;
    }

    while (rest >= 10u) {
        rest /= 10u;
        digits++;
    }

    keep = 8u - 1u - digits;
    if (keep > blen) {
        keep = blen;
    }

    memset(out, ' ', 11);
    memcpy(out, name11, keep);
    out[keep] = (k_u8_t)'~';

    rest = number;
    for (k_size_t i = digits; i > 0; i--) {
        out[keep + i] = (k_u8_t)('0' + (rest % 10u));
        rest /= 10u;
    }

    memcpy(out + 8, name11 + 8, elen);
}

static int unique_short_name(struct fat_volume *vol, k_u32_t dir_cluster, k_u8_t name11[11],
                             bool *changed)
{
    k_u8_t candidate[11];
    bool found;
    k_u32_t number;
    int rc;

    *changed = false;

    rc = short_exists(vol, dir_cluster, name11, &found);
    if (rc != 0 || !found) {
        return rc;
    }

    for (number = 1u; number < 1000000u; number++) {
        short_variant(name11, number, candidate);
        rc = short_exists(vol, dir_cluster, candidate, &found);
        if (rc != 0) {
            return rc;
        }
        if (!found) {
            memcpy(name11, candidate, 11);
            *changed = true;
            return 0;
        }
    }

    return KERR_NOSPC;
}

/* UTF-8 to the UTF-16 units of a long name, surrogates included. */
static int utf8_to_utf16(const char *src, k_u16_t *out, k_size_t cap, k_size_t *count)
{
    const k_u8_t *p = (const k_u8_t *)src;
    k_size_t used = 0;

    while (*p != 0) {
        k_u32_t cp;

        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xE0u) == 0xC0u) {
            if ((p[1] & 0xC0u) != 0x80u) {
                return KERR_INVAL;
            }
            cp = ((k_u32_t)(p[0] & 0x1Fu) << 6) | (k_u32_t)(p[1] & 0x3Fu);
            if (cp < 0x80u) {
                return KERR_INVAL;
            }
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u) {
            if (p[1] == 0 || (p[1] & 0xC0u) != 0x80u || p[2] == 0 || (p[2] & 0xC0u) != 0x80u) {
                return KERR_INVAL;
            }
            cp = ((k_u32_t)(p[0] & 0x0Fu) << 12) | ((k_u32_t)(p[1] & 0x3Fu) << 6) |
                 (k_u32_t)(p[2] & 0x3Fu);
            if (cp < 0x800u) {
                return KERR_INVAL;
            }
            p += 3;
        } else if ((*p & 0xF8u) == 0xF0u) {
            if (p[1] == 0 || (p[1] & 0xC0u) != 0x80u || p[2] == 0 || (p[2] & 0xC0u) != 0x80u ||
                p[3] == 0 || (p[3] & 0xC0u) != 0x80u) {
                return KERR_INVAL;
            }
            cp = ((k_u32_t)(p[0] & 0x07u) << 18) | ((k_u32_t)(p[1] & 0x3Fu) << 12) |
                 ((k_u32_t)(p[2] & 0x3Fu) << 6) | (k_u32_t)(p[3] & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                return KERR_INVAL;
            }
            p += 4;
        } else {
            return KERR_INVAL;
        }

        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            if (used + 2u > cap) {
                return KERR_NAMETOOLONG;
            }
            out[used++] = (k_u16_t)(0xD800u | (cp >> 10));
            out[used++] = (k_u16_t)(0xDC00u | (cp & 0x3FFu));
        } else {
            if (used + 1u > cap) {
                return KERR_NAMETOOLONG;
            }
            out[used++] = (k_u16_t)cp;
        }
    }

    *count = used;
    return 0;
}

static void build_lfn_entry(k_u8_t raw[32], k_u8_t sequence, bool last, k_u8_t checksum,
                            const k_u16_t *units, k_size_t count, k_size_t base)
{
    k_size_t i;

    memset(raw, 0, 32);
    raw[0] = (k_u8_t)(sequence | (last ? 0x40u : 0u));
    raw[11] = FAT_ATTR_LFN;
    raw[13] = checksum;

    for (i = 0; i < LFN_UNITS_PER_ENTRY; i++) {
        k_size_t position = base + i;
        k_u16_t unit;
        k_size_t offset;

        if (position < count) {
            unit = units[position];
        } else if (position == count) {
            unit = 0x0000u;
        } else {
            unit = 0xFFFFu;
        }

        if (i < LFN_CHARS_1) {
            offset = 1u + i * 2u;
        } else if (i < LFN_CHARS_1 + LFN_CHARS_2) {
            offset = 14u + (i - LFN_CHARS_1) * 2u;
        } else {
            offset = 28u + (i - LFN_CHARS_1 - LFN_CHARS_2) * 2u;
        }

        fat_put16(raw + offset, unit);
    }
}

static void build_short_entry(k_u8_t raw[32], const k_u8_t name11[11], k_u8_t attr, k_u32_t cluster,
                              k_u32_t size)
{
    k_u16_t date;
    k_u16_t time;

    fat_time_encode(k_clock_now_unix(), &date, &time);
    memset(raw, 0, 32);
    memcpy(raw, name11, 11);
    raw[11] = attr;
    fat_put16(raw + 20, (k_u16_t)(cluster >> 16));
    fat_put16(raw + 22, time);
    fat_put16(raw + 24, date);
    fat_put16(raw + 26, (k_u16_t)(cluster & 0xFFFFu));
    fat_put32(raw + 28, size);
}

int fat_dir_create(struct fat_volume *vol, k_u32_t dir_cluster, const char *name, k_u8_t attr,
                   k_u32_t first_cluster, k_u32_t size, k_u32_t *pos)
{
    k_u8_t name11[11];
    k_u8_t checksum;
    k_u8_t raw[32];
    k_u16_t units[FAT_LFN_UNITS];
    k_size_t unit_count = 0;
    k_size_t lfn_entries = 0;
    k_u32_t start;
    k_u32_t at;
    k_u32_t needed;
    k_u32_t i;
    bool plain = true;
    bool renamed = false;
    int rc;

    if (vol == NULL || name == NULL || name[0] == '\0') {
        return KERR_INVAL;
    }

    if (strlen(name) > FAT_NAME_MAX) {
        return KERR_NAMETOOLONG;
    }

    make_short_name(name, name11, &plain);

    rc = unique_short_name(vol, dir_cluster, name11, &renamed);
    if (rc != 0) {
        return rc;
    }
    if (renamed) {
        plain = false;
    }

    if (!plain) {
        rc = utf8_to_utf16(name, units, 255u, &unit_count);
        if (rc != 0) {
            return rc;
        }
        if (unit_count == 0) {
            return KERR_INVAL;
        }

        lfn_entries = (unit_count + LFN_UNITS_PER_ENTRY - 1u) / LFN_UNITS_PER_ENTRY;
        if (lfn_entries > FAT_LFN_ENTRIES) {
            return KERR_NAMETOOLONG;
        }
    }

    needed = (k_u32_t)lfn_entries + 1u;
    rc = dir_find_slots(vol, dir_cluster, needed, &start);
    if (rc != 0) {
        return rc;
    }

    at = start;
    checksum = short_checksum(name11);

    for (i = (k_u32_t)lfn_entries; i > 0; i--) {
        build_lfn_entry(raw, (k_u8_t)i, i == (k_u32_t)lfn_entries, checksum, units, unit_count,
                        (k_size_t)(i - 1u) * LFN_UNITS_PER_ENTRY);
        rc = dir_write_raw(vol, dir_cluster, at, raw);
        if (rc != 0) {
            return rc;
        }
        at += 32u;
    }

    build_short_entry(raw, name11, attr, first_cluster, size);
    rc = dir_write_raw(vol, dir_cluster, at, raw);
    if (rc != 0) {
        return rc;
    }

    if (pos != NULL) {
        *pos = at;
    }

    return 0;
}

int fat_dir_set(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t pos, k_u32_t expected_first,
                k_u32_t first_cluster, k_u32_t size)
{
    k_u8_t raw[32];
    k_u32_t on_medium;
    k_u16_t date;
    k_u16_t time;
    int rc;

    if (vol == NULL) {
        return KERR_INVAL;
    }

    rc = dir_read_raw(vol, dir_cluster, pos, raw);
    if (rc != 0) {
        return rc;
    }

    on_medium = (k_u32_t)fat_le16(raw + 26) | ((k_u32_t)fat_le16(raw + 20) << 16);
    if (on_medium != expected_first) {
        return KERR_NOENT;
    }

    /* The content of the entry moved, so its moment moves with it. */
    fat_time_encode(k_clock_now_unix(), &date, &time);
    fat_put16(raw + 20, (k_u16_t)(first_cluster >> 16));
    fat_put16(raw + 22, time);
    fat_put16(raw + 24, date);
    fat_put16(raw + 26, (k_u16_t)(first_cluster & 0xFFFFu));
    fat_put32(raw + 28, size);

    return dir_write_raw(vol, dir_cluster, pos, raw);
}

int fat_dir_init(struct fat_volume *vol, k_u32_t cluster, k_u32_t parent_cluster)
{
    k_u8_t name11[11];
    k_u8_t raw[32];
    int rc;

    if (vol == NULL || cluster < 2u) {
        return KERR_INVAL;
    }

    memset(name11, ' ', 11);
    name11[0] = (k_u8_t)'.';
    build_short_entry(raw, name11, FAT_ATTR_DIR, cluster, 0);
    rc = dir_write_raw(vol, cluster, 0, raw);
    if (rc != 0) {
        return rc;
    }

    memset(name11, ' ', 11);
    name11[0] = (k_u8_t)'.';
    name11[1] = (k_u8_t)'.';
    build_short_entry(raw, name11, FAT_ATTR_DIR, parent_cluster, 0);
    return dir_write_raw(vol, cluster, 32u, raw);
}

/* A directory is empty when it holds no entry besides "." and "..". */
static int dir_is_empty(struct fat_volume *vol, k_u32_t dir_cluster, bool *empty)
{
    struct fat_entry entry;
    k_u32_t pos = 0;
    int rc;

    *empty = true;

    if (dir_cluster < 2u) {
        return KERR_IO;
    }

    rc = fat_scan_next(vol, dir_cluster, &pos, &entry);
    if (rc < 0) {
        return rc;
    }

    *empty = (rc == 0);
    return 0;
}

/* Mark the short entry and the long-name run before it as removed. */
static int remove_entry_run(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t entry_pos)
{
    k_u32_t start = entry_pos;
    k_u32_t at;
    k_u8_t raw[32];
    int rc;

    while (start >= 32u) {
        rc = dir_read_raw(vol, dir_cluster, start - 32u, raw);
        if (rc != 0) {
            return rc;
        }
        if (raw[11] != FAT_ATTR_LFN || raw[0] == 0xE5) {
            break;
        }
        start -= 32u;
    }

    for (at = start; at <= entry_pos; at += 32u) {
        rc = dir_read_raw(vol, dir_cluster, at, raw);
        if (rc != 0) {
            return rc;
        }
        raw[0] = 0xE5;
        rc = dir_write_raw(vol, dir_cluster, at, raw);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

int fat_dir_remove(struct fat_volume *vol, const char *path)
{
    struct fat_entry parent;
    struct fat_entry entry;
    char leaf[FAT_NAME_MAX + 1];
    k_u32_t entry_pos = 0;
    int rc;
    if (vol == NULL || path == NULL) {
        return KERR_INVAL;
    }

    rc = fat_lookup_parent(vol, path, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }

    rc = find_in_dir(vol, parent.cluster, leaf, &entry, &entry_pos);
    if (rc != 0) {
        return rc;
    }

    if (entry.dir) {
        bool empty;

        rc = dir_is_empty(vol, entry.cluster, &empty);
        if (rc != 0) {
            return rc;
        }
        if (!empty) {
            return KERR_NOTEMPTY;
        }
    }

    /* The entry goes first and the chain after it: a failure between the two
     * leaves clusters that nobody names, which a checker can find, and never
     * an entry that points at clusters the free map has already taken. */
    rc = remove_entry_run(vol, parent.cluster, entry_pos);
    if (rc != 0) {
        return rc;
    }

    if (entry.cluster >= 2u) {
        rc = fat_chain_free(vol, entry.cluster);
    }

    return rc;
}

/*
 * The same object under another name (ADR 0024 §5). FAT has no rename in
 * its format: a name is a run of entries in a directory, so the call writes
 * the run of the new name and marks the old one removed. The new name comes
 * first, because a volume that loses the power between the two has two names
 * of one chain - which the checker reports and a person can remove - and
 * never an object that no name reaches.
 *
 * A file may move to another directory of the volume. A directory stays in
 * its parent: its own "." and ".." entries and the tree below it name the
 * place it came from, and rewriting them is a walk this call does not make.
 */
int fat_dir_rename(struct fat_volume *vol, const char *from, const char *to)
{
    struct fat_entry from_parent;
    struct fat_entry to_parent;
    struct fat_entry entry;
    struct fat_entry existing;
    char from_leaf[FAT_NAME_MAX + 1];
    char to_leaf[FAT_NAME_MAX + 1];
    k_u32_t from_pos = FAT_POS_NONE;
    k_u32_t to_pos = FAT_POS_NONE;
    k_u32_t created = FAT_POS_NONE;
    int rc;

    if (vol == NULL || from == NULL || to == NULL) {
        return KERR_INVAL;
    }

    rc = fat_lookup_parent(vol, from, &from_parent, from_leaf, sizeof(from_leaf));
    if (rc != 0) {
        return rc;
    }

    if (!from_parent.dir) {
        return KERR_NOTDIR;
    }

    rc = find_in_dir(vol, from_parent.cluster, from_leaf, &entry, &from_pos);
    if (rc != 0) {
        return rc;
    }

    rc = fat_lookup_parent(vol, to, &to_parent, to_leaf, sizeof(to_leaf));
    if (rc != 0) {
        return rc;
    }

    if (!to_parent.dir) {
        return KERR_NOTDIR;
    }

    if (entry.dir && from_parent.cluster != to_parent.cluster) {
        return KERR_NOTSUP;
    }

    /*
     * FAT compares names without their case, so "HELLO" and "hello" are one
     * name. A name that is already there is replaced when it is a file and
     * refused when it is a directory, like the VFS says.
     */
    rc = find_in_dir(vol, to_parent.cluster, to_leaf, &existing, &to_pos);
    if (rc == 0) {
        if (existing.dir) {
            return KERR_ISDIR;
        }

        rc = remove_entry_run(vol, to_parent.cluster, to_pos);
        if (rc != 0) {
            return rc;
        }

        /* The name of the file that was here is gone, and its data goes
         * with it: a chain that no name reaches is a lost chain, which a
         * checker reports and a person has to repair by hand. */
        if (existing.cluster >= 2u) {
            rc = fat_chain_free(vol, existing.cluster);
            if (rc != 0) {
                return rc;
            }
        }
    } else if (rc != KERR_NOENT) {
        return rc;
    }

    rc = fat_dir_create(vol, to_parent.cluster, to_leaf, (k_u8_t)(entry.attr & ~FAT_ATTR_LFN),
                        entry.cluster, entry.size, &created);
    if (rc != 0) {
        return rc;
    }

    rc = remove_entry_run(vol, from_parent.cluster, from_pos);
    if (rc != 0) {
        /* The old name is still the truth: the new one was a name too many,
         * so it goes back and the caller gets the error of the removal. */
        (void)remove_entry_run(vol, to_parent.cluster, created);
        return rc;
    }

    return 0;
}
