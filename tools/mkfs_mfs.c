/*
 * Micro OS — the command line of the image builder.
 *
 *   microos_mkfs --image disk.img --dir /bin --program /bin/hello=hello.mox
 *                --text /etc/setting=hello
 *
 * The tool only parses the line and reads the files of the host; the image
 * itself is written by mkfs_mfs_image.c, which speaks the block device and
 * lets components/mfs put the objects into the volume.
 */
#include "mkfs_mfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/types.h"

struct mkfs_args {
    const char *image;
    k_u32_t size_mb;
    k_u32_t start_lba;
    k_u64_t epoch;
    struct mkfs_mfs_entry *entries;
    k_size_t count;
    k_size_t capacity;
    char **owned;
    k_size_t owned_count;
    k_size_t owned_capacity;
};

static void usage(void)
{
    printf("usage: microos_mkfs --image PATH [options]\n"
           "\n"
           "  --image PATH            write the image here\n"
           "  --size-mb N             the size of the image, 8 by default\n"
           "  --start-lba N           the first sector of the partition, 2048 by default\n"
           "  --source-date-epoch N   the moment of the records, $SOURCE_DATE_EPOCH, 0\n"
           "  --dir PATH              make a directory of the volume\n"
           "  --file PATH=HOST        write a file of the host (mode 0644)\n"
           "  --program PATH=HOST     write a program (mode 0755)\n"
           "  --text PATH=TEXT        write TEXT and a newline (mode 0644)\n"
           "  --volumes LIST          name the volumes of this system in /etc/volumes:\n"
           "                          one device name per comma (ahci0p1), or `none`\n"
           "  --mode MODE             the mode of the entry just written, in octal\n"
           "                          (0600 for a file only its owner reads)\n"
           "  --help                  this text\n");
}

/* A number of seconds, or false when the text is not one. */
static bool parse_epoch(const char *text, k_u64_t *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }

    value = strtoull(text, &end, 10);

    if (end == text || *end != '\0') {
        return false;
    }

    *out = (k_u64_t)value;

    return true;
}

static bool reserve_entry(struct mkfs_args *args)
{
    struct mkfs_mfs_entry *grown;
    k_size_t capacity;

    if (args->count < args->capacity) {
        return true;
    }

    capacity = args->capacity == 0 ? 16 : args->capacity * 2;
    grown = (struct mkfs_mfs_entry *)realloc(args->entries, capacity * sizeof(*grown));

    if (grown == NULL) {
        fprintf(stderr, "mkfs: out of memory for the list of the objects\n");
        return false;
    }

    args->entries = grown;
    args->capacity = capacity;

    return true;
}

static bool keep_owned(struct mkfs_args *args, char *text)
{
    char **grown;
    k_size_t capacity;

    if (args->owned_count == args->owned_capacity) {
        capacity = args->owned_capacity == 0 ? 16 : args->owned_capacity * 2;
        grown = (char **)realloc(args->owned, capacity * sizeof(*grown));

        if (grown == NULL) {
            fprintf(stderr, "mkfs: out of memory for the text of the objects\n");
            return false;
        }

        args->owned = grown;
        args->owned_capacity = capacity;
    }

    args->owned[args->owned_count++] = text;

    return true;
}

static bool add_entry(struct mkfs_args *args, const char *path, const char *source,
                      const k_u8_t *data, k_size_t size, k_u32_t mode, bool directory)
{
    struct mkfs_mfs_entry *entry;

    if (path == NULL || path[0] != '/') {
        fprintf(stderr, "mkfs: a path of the volume has to start with '/'\n");
        return false;
    }

    if (!reserve_entry(args)) {
        return false;
    }

    entry = &args->entries[args->count++];
    memset(entry, 0, sizeof(*entry));
    entry->path = path;
    entry->source = source;
    entry->data = data;
    entry->size = size;
    entry->mode = mode;
    entry->directory = directory;

    return true;
}

/* "PATH=VALUE": the path is up to the first '=', the value is the rest. */
static bool split_pair(char *argument, char **path, char **value)
{
    char *equals = strchr(argument, '=');

    if (equals == NULL || equals == argument) {
        fprintf(stderr, "mkfs: '%s' is not PATH=VALUE\n", argument);
        return false;
    }

    *equals = '\0';
    *path = argument;
    *value = equals + 1;

    return true;
}

/*
 * `/etc/volumes` of the volume just written (ADR 0041 §1): the list the rule
 * of the disks reads to decide whether a volume may be written. `mkfs` knows
 * which device it formatted - the caller passes the names - and nothing else
 * in the system writes this file.
 *
 * The list is one device name per line, the shape the rule parses; a comma is
 * the separator of the command line and becomes a newline. `none` (or an
 * empty list) writes no file: a medium that names no volume is read-only,
 * which is the safe default of a tool that was not told what it made.
 */
static bool add_volumes(struct mkfs_args *args, const char *list)
{
    static const char header[] = "# the volumes of this system that a mount may write (ADR 0041)\n";
    size_t header_length = sizeof(header) - 1u;
    size_t length;
    char *copy;
    size_t i;

    if (list == NULL || list[0] == '\0' || strcmp(list, "none") == 0) {
        return true;
    }

    length = strlen(list);

    /* A name that is not a word of a device is not written: the rule would
     * read a line of nonsense and refuse the volume it was meant to allow. */
    for (i = 0; i < length; i++) {
        if (list[i] == ',') {
            continue;
        }

        if (list[i] == ' ' || list[i] == '\t') {
            fprintf(stderr, "mkfs: --volumes takes device names separated by commas\n");
            return false;
        }
    }

    copy = (char *)malloc(header_length + length + 2u);

    if (copy == NULL) {
        fprintf(stderr, "mkfs: out of memory for the list of the volumes\n");
        return false;
    }

    memcpy(copy, header, header_length);

    for (i = 0; i < length; i++) {
        copy[header_length + i] = list[i] == ',' ? '\n' : list[i];
    }

    copy[header_length + length] = '\n';
    copy[header_length + length + 1u] = '\0';

    if (!keep_owned(args, copy)) {
        free(copy);
        return false;
    }

    return add_entry(args, "/etc/volumes", NULL, (const k_u8_t *)copy,
                     (k_size_t)(header_length + length + 1u), MKFS_MFS_MODE_FILE, false);
}

static bool add_text(struct mkfs_args *args, char *pair)
{
    char *path;
    char *text;
    size_t length;
    char *copy;

    if (!split_pair(pair, &path, &text)) {
        return false;
    }

    length = strlen(text);
    copy = (char *)malloc(length + 2u);

    if (copy == NULL) {
        fprintf(stderr, "mkfs: out of memory for the text of %s\n", path);
        return false;
    }

    memcpy(copy, text, length);
    copy[length] = '\n';
    copy[length + 1u] = '\0';

    if (!keep_owned(args, copy)) {
        free(copy);
        return false;
    }

    return add_entry(args, path, NULL, (const k_u8_t *)copy, (k_size_t)(length + 1u),
                     MKFS_MFS_MODE_FILE, false);
}

int main(int argc, char **argv)
{
    struct mkfs_args args;
    int index;
    int rc;

    memset(&args, 0, sizeof(args));
    args.size_mb = 8;
    args.start_lba = MKFS_MFS_START_LBA;
    args.epoch = MKFS_MFS_SOURCE_DATE_EPOCH;

    /* The convention of a reproducible build: the environment names one
     * moment, and the option below has the last word. */
    {
        const char *environment = getenv("SOURCE_DATE_EPOCH");

        if (environment != NULL && !parse_epoch(environment, &args.epoch)) {
            fprintf(stderr, "mkfs: SOURCE_DATE_EPOCH is not a number: %s\n", environment);
            return 2;
        }
    }

    for (index = 1; index < argc; index++) {
        const char *option = argv[index];
        const char *value = index + 1 < argc ? argv[index + 1] : NULL;

        if (strcmp(option, "--help") == 0) {
            usage();
            free(args.entries);
            return 0;
        }

        if (value == NULL) {
            fprintf(stderr, "mkfs: %s needs a value\n", option);
            rc = 2;
            goto out;
        }

        if (strcmp(option, "--image") == 0) {
            args.image = value;
        } else if (strcmp(option, "--size-mb") == 0) {
            args.size_mb = (k_u32_t)strtoul(value, NULL, 10);
        } else if (strcmp(option, "--start-lba") == 0) {
            args.start_lba = (k_u32_t)strtoul(value, NULL, 10);
        } else if (strcmp(option, "--source-date-epoch") == 0) {
            if (!parse_epoch(value, &args.epoch)) {
                fprintf(stderr, "mkfs: %s is not a number of seconds\n", value);
                rc = 2;
                goto out;
            }
        } else if (strcmp(option, "--dir") == 0) {
            if (!add_entry(&args, value, NULL, NULL, 0, MKFS_MFS_MODE_DIR, true)) {
                rc = 1;
                goto out;
            }
        } else if (strcmp(option, "--file") == 0 || strcmp(option, "--program") == 0) {
            char *path;
            char *source;
            k_u32_t mode =
                strcmp(option, "--program") == 0 ? MKFS_MFS_MODE_PROGRAM : MKFS_MFS_MODE_FILE;

            if (!split_pair((char *)value, &path, &source)) {
                rc = 2;
                goto out;
            }

            if (!add_entry(&args, path, source, NULL, 0, mode, false)) {
                rc = 1;
                goto out;
            }
        } else if (strcmp(option, "--text") == 0) {
            if (!add_text(&args, (char *)value)) {
                rc = 1;
                goto out;
            }
        } else if (strcmp(option, "--volumes") == 0) {
            if (!add_volumes(&args, value)) {
                rc = 2;
                goto out;
            }
        } else if (strcmp(option, "--mode") == 0) {
            /*
             * The mode of the entry that was just named: the list is one
             * object after another, and a mode belongs to the object before
             * it. A file of a volume that only its owner reads (/etc/shadow)
             * is written as `--text PATH=TEXT --mode 0600`.
             */
            if (args.count == 0) {
                fprintf(stderr, "mkfs: --mode comes after the entry it is the mode of\n");
                rc = 2;
                goto out;
            }

            args.entries[args.count - 1].mode = (k_u32_t)strtoul(value, NULL, 8);
        } else {
            fprintf(stderr, "mkfs: unknown option %s\n", option);
            usage();
            rc = 2;
            goto out;
        }

        index++;
    }

    if (args.image == NULL) {
        fprintf(stderr, "mkfs: --image is not given\n");
        rc = 2;
        goto out;
    }

    rc = mkfs_mfs_build_image(args.image, args.size_mb, args.start_lba, args.entries, args.count,
                              args.epoch) == 0
             ? 0
             : 1;

    if (rc == 0) {
        printf("mkfs: %s, %u MB, %u object(s)\n", args.image, (unsigned)args.size_mb,
               (unsigned)args.count);
    }

out:
    while (args.owned_count > 0) {
        free(args.owned[--args.owned_count]);
    }
    free(args.owned);
    free(args.entries);

    return rc;
}
