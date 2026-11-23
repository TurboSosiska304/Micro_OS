/*
 * Micro OS — /bin/file, the type of a file.
 *
 * The library is libmagic of file 5.45 (third_party/file) and the database is
 * the compact one the build writes into the image: /etc/magic.mgc, compiled
 * from the subset of Magdir, with /etc/magic (the same text) as the fallback
 * when the compiled file is missing. The program is ours: the command of the
 * release parses its options with getopt_long, prints through a stream and
 * decompresses with a child process, and none of the three is a shape this
 * machine has.
 *
 * The machine has no `mmap`: `magic_buffer()` is handed the bytes the program
 * read into the arena, and the database is read whole by the library
 * (apprentice_map allocates the file and reads it, without QUICK). The file
 * is read to a limit of FILE_BUFFER_MAX; the magic rules of the subset look
 * at the first bytes and at a few offsets near the end, which is enough for
 * the formats of the database and for the RAM of the machine.
 *
 *   file [-b] [-i] [-L] [-z] [--] FILE...
 *   file [-b] [-i] [-z] -f LIST
 *   file -            the bytes of descriptor 0
 *
 *   -b   no name before the description
 *   -i   the MIME type and the encoding of the text
 *   -L   follow a symbolic link (the VFS has none: the flag is accepted)
 *   -z   look inside a gzip file (the library of zlib of the image)
 *   -f   the names are the lines of LIST, one name per line
 *
 * The status is 0 when every name was read and 1 when one of them was not:
 * the sentence of the failure names the call, as the command of the release
 * does (`cannot open `x' (No such file or directory)`).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <magic.h>
#include <zlib.h>

/* The bytes of the file that the library may see, and of a gzip it opens. */
#define FILE_BUFFER_MAX (128u * 1024u)
#define FILE_GZIP_MAX (128u * 1024u)

/* The names of the two gzip bytes, and the window of inflate for gzip. */
#define FILE_GZIP_0 0x1f
#define FILE_GZIP_1 0x8b
#define FILE_GZIP_WINDOW (15 + 16)

static magic_t g_cookie;
static int g_brief;
static int g_mime;
static int g_inside;
static int g_failed;

static void usage(void)
{
    (void)fputs("usage: file [-b] [-i] [-L] [-z] [-f list] [--] file...\n", stderr);
}

/* Read a descriptor into the block of the caller. A pipe of this system
 * answers EAGAIN while its producer is there (ADR 0014), which is not the end
 * of the stream: the read waits the condition out. */
static size_t read_all(int fd, unsigned char *buffer, size_t size)
{
    size_t used = 0;

    while (used < size) {
        size_t want = size - used;
        ssize_t got;

        /* The edge of one transfer of the system (KSYS_MAX_IO): a read of a
         * larger count is EINVAL, not a short read. */
        if (want > KSYS_MAX_IO) {
            want = KSYS_MAX_IO;
        }
        got = read(fd, buffer + used, want);

        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            return (size_t)-1;
        }
        used += (size_t)got;
    }
    return used;
}

/*
 * Inflate a gzip stream into the block of the caller. The library is zlib of
 * the image and the call is the raw one of `inflateInit2` with a window of 16
 * bits more than the deflate one, which is the flag of a gzip wrapper.
 */
static size_t gunzip(const unsigned char *source, size_t length, unsigned char *out, size_t size)
{
    z_stream stream;
    int rc;
    size_t used;

    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, FILE_GZIP_WINDOW) != Z_OK) {
        return (size_t)-1;
    }
    stream.next_in = (Bytef *)source;
    stream.avail_in = (uInt)length;
    stream.next_out = out;
    stream.avail_out = (uInt)size;
    rc = inflate(&stream, Z_FINISH);
    used = stream.total_out;
    (void)inflateEnd(&stream);
    if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        return (size_t)-1;
    }
    return used;
}

/*
 * The sentence of an error code as the command of the release prints it: the
 * text of the library with its first letter in the upper case. The library of
 * this system speaks lower case (`no such file or directory`), and the
 * sentences of a command are a contract its callers read.
 */
static const char *reason_text(int code)
{
    static char text[64];
    const char *reason = strerror(code);
    size_t length = strlen(reason);

    if (length >= sizeof(text)) {
        length = sizeof(text) - 1;
    }
    memcpy(text, reason, length);
    text[length] = '\0';
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] = (char)(text[0] - 'a' + 'A');
    }
    return text;
}

static void report(const char *name, const char *description)
{
    if (g_brief) {
        (void)printf("%s\n", description);
    } else {
        (void)printf("%s: %s\n", name, description);
    }
}

/* A directory is not read: the local magic of the library names it, and the
 * MIME type is the one of a directory. */
static void report_directory(const char *name)
{
    report(name, g_mime ? "inode/directory" : "directory");
}

static void describe(const char *name)
{
    unsigned char *buffer;
    unsigned char *inflated;
    size_t used;
    const char *description;
    int fd;

    if (strcmp(name, "-") == 0) {
        fd = ULIBC_STDIN;
    } else {
        struct stat info;

        if (stat(name, &info) != 0) {
            (void)fprintf(stderr, "file: cannot open `%s' (%s)\n", name, reason_text(errno));
            g_failed = 1;
            return;
        }
        if (S_ISDIR(info.st_mode)) {
            report_directory(name);
            return;
        }
        fd = open(name, O_RDONLY);
        if (fd < 0) {
            errno = -fd;
            (void)fprintf(stderr, "file: cannot open `%s' (%s)\n", name, reason_text(errno));
            g_failed = 1;
            return;
        }
    }

    buffer = malloc(FILE_BUFFER_MAX);
    if (buffer == NULL) {
        (void)fprintf(stderr, "file: out of memory\n");
        g_failed = 1;
        if (fd != ULIBC_STDIN) {
            (void)close(fd);
        }
        return;
    }

    used = read_all(fd, buffer, FILE_BUFFER_MAX);
    if (fd != ULIBC_STDIN) {
        (void)close(fd);
    }
    if (used == (size_t)-1) {
        (void)fprintf(stderr, "file: cannot read `%s' (%s)\n", name, reason_text(errno));
        g_failed = 1;
        free(buffer);
        return;
    }

    if (g_inside && used >= 2 && buffer[0] == FILE_GZIP_0 && buffer[1] == FILE_GZIP_1) {
        char text[256];

        inflated = malloc(FILE_GZIP_MAX);
        if (inflated != NULL) {
            size_t total = gunzip(buffer, used, inflated, FILE_GZIP_MAX);

            if (total != (size_t)-1) {
                description = magic_buffer(g_cookie, inflated, total);
                if (description != NULL) {
                    (void)snprintf(text, sizeof(text), "%s (gzip compressed data)", description);
                    report(name, text);
                    free(inflated);
                    free(buffer);
                    return;
                }
            }
            free(inflated);
        }
    }

    description = magic_buffer(g_cookie, buffer, used);
    report(name, description != NULL ? description : "data");
    free(buffer);
}

static void describe_list(const char *path)
{
    FILE *list;
    char line[256];

    list = fopen(path, "r");
    if (list == NULL) {
        (void)fprintf(stderr, "file: cannot open `%s' (%s)\n", path, reason_text(errno));
        g_failed = 1;
        return;
    }
    while (fgets(line, sizeof(line), list) != NULL) {
        size_t length = strlen(line);

        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length > 0) {
            describe(line);
        }
    }
    (void)fclose(list);
}

/* The options of a line: the flags are one letter each and may be clustered
 * (`-bi`), and `--` ends the options. `-f` names the next word. */
static int parse_options(int argc, char **argv, const char **list)
{
    int index;
    int files = 0;

    for (index = 1; index < argc; index++) {
        const char *word = argv[index];

        if (strcmp(word, "--") == 0) {
            index++;
            break;
        }
        if (word[0] != '-' || word[1] == '\0') {
            break;
        }
        for (int letter = 1; word[letter] != '\0'; letter++) {
            switch (word[letter]) {
            case 'b':
                g_brief = 1;
                break;
            case 'i':
                g_mime = 1;
                break;
            case 'L':
                /* The VFS of this system has no symbolic link to follow. */
                break;
            case 'z':
                g_inside = 1;
                break;
            case 'f':
                if (word[letter + 1] != '\0' || index + 1 >= argc) {
                    usage();
                    return -1;
                }
                *list = argv[++index];
                break;
            default:
                usage();
                return -1;
            }
            if (*list != NULL && word[letter] == 'f') {
                break;
            }
        }
    }

    for (; index < argc; index++) {
        files++;
    }
    return files;
}

int main(int argc, char **argv, char *const envp[])
{
    const char *list = NULL;
    const char *magic_file;
    int files;

    (void)envp;

    files = parse_options(argc, argv, &list);
    if (files < 0) {
        return 1;
    }

    /* The two levels of the database: the compiled /etc/magic.mgc first and
     * the text /etc/magic when the compiled file is not there. `magic_load`
     * with the name of the text tries the .mgc beside it itself, so the path
     * is the text and the two levels are one call. */
    magic_file = getenv("MAGIC");
    g_cookie = magic_open(MAGIC_NONE);
    if (g_cookie == NULL) {
        (void)fprintf(stderr, "file: cannot open magic database\n");
        return 1;
    }
    if (magic_load(g_cookie, magic_file) != 0) {
        const char *reason = magic_error(g_cookie);

        (void)fprintf(stderr, "file: cannot open magic database%s%s\n",
                      (reason != NULL && reason[0] != '\0') ? ": " : "",
                      (reason != NULL && reason[0] != '\0') ? reason : "");
        magic_close(g_cookie);
        return 1;
    }
    /* The flags of a call are the ones of the option: the database is loaded
     * once and the answer of each name is the mode of the line. */
    (void)magic_setflags(g_cookie, g_mime ? MAGIC_MIME : MAGIC_NONE);

    if (list != NULL) {
        describe_list(list);
    } else if (files == 0) {
        usage();
        magic_close(g_cookie);
        return 1;
    } else {
        /* The names are the words after the options: `parse_options` left
         * `argc`/`argv` untouched, so walk the line again from the end. */
        int first = argc - files;

        for (int index = first; index < argc; index++) {
            describe(argv[index]);
        }
    }

    magic_close(g_cookie);
    return g_failed ? 1 : 0;
}
