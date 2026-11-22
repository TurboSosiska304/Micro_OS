/*
 * Micro OS — the parser of the directory of the `progs` partition.
 *
 * The bytes are read field by field and never cast to a structure: the
 * partition is data of the flash and may carry anything, so a table with a
 * count of a billion must be refused before it is walked. The contract of
 * the format and the reasons of each check are in progs_format.h.
 */
#include "progs_format.h"

#include <string.h>

static k_u32_t read_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

/* A name a path can carry: a letter, a digit, a dot, a dash or an underscore. */
static bool name_is_valid(const char *name, k_size_t length)
{
    k_size_t i;

    if (length == 0 || length > PROGS_NAME_MAX) {
        return false;
    }

    /* The two names of the tree are not files of a directory. */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    for (i = 0; i < length; i++) {
        char c = name[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_') {
            continue;
        }

        return false;
    }

    return true;
}

int progs_directory_parse(const k_u8_t *data, k_size_t size, k_u32_t partition_size,
                          struct progs_entry *out, k_size_t max, k_size_t *count)
{
    k_u32_t slot;
    k_u32_t files;
    k_u32_t area;
    k_size_t found = 0;
    k_u32_t index;

    if (count != NULL) {
        *count = 0;
    }

    if (data == NULL || out == NULL || count == NULL || max == 0) {
        return -1;
    }

    /* The program area is the partition less the 4 KiB of the directory. */
    if (partition_size <= PROGS_DIRECTORY_SIZE) {
        return -1;
    }
    area = partition_size - PROGS_DIRECTORY_SIZE;

    if (size < PROGS_TABLE_HEADER_SIZE || memcmp(data, PROGS_MAGIC, PROGS_MAGIC_SIZE) != 0) {
        return -1;
    }

    if (read_le32(data + 8) != PROGS_VERSION) {
        return -1;
    }

    files = read_le32(data + 12);
    if (files > PROGS_MAX_FILES || files > max) {
        return -1;
    }

    slot = read_le32(data + 16);
    if (slot < PROGS_SLOT_SIZE || slot % PROGS_SLOT_SIZE != 0) {
        return -1;
    }

    if (size < PROGS_TABLE_HEADER_SIZE + (k_size_t)files * PROGS_ENTRY_SIZE) {
        return -1;
    }

    for (index = 0; index < files; index++) {
        const k_u8_t *record = data + PROGS_TABLE_HEADER_SIZE + (k_size_t)index * PROGS_ENTRY_SIZE;
        char name[PROGS_NAME_MAX + 1u];
        k_u32_t offset;
        k_u32_t length;
        k_size_t name_length;
        k_size_t i;

        name_length = 0;
        while (name_length < PROGS_NAME_MAX + 1u && record[name_length] != '\0') {
            name_length++;
        }
        if (name_length > PROGS_NAME_MAX || name_length >= sizeof(name)) {
            return -1;
        }

        memcpy(name, record, name_length);
        name[name_length] = '\0';

        if (!name_is_valid(name, name_length)) {
            return -1;
        }

        offset = read_le32(record + 24);
        length = read_le32(record + 28);

        if (length == 0 || length > slot) {
            return -1;
        }

        /* Every file on a slot boundary of its own, and none of them over the
         * directory at the end. The first program has offset 0. */
        if (offset % slot != 0) {
            return -1;
        }

        if (offset > area || length > area - offset) {
            return -1;
        }

        for (i = 0; i < found; i++) {
            if (strcmp(out[i].name, name) == 0) {
                return -1;
            }
        }

        memcpy(out[found].name, name, name_length + 1u);
        out[found].offset = offset;
        out[found].size = length;
        found++;
    }

    *count = found;

    return 0;
}
