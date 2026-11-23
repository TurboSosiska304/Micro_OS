/*
 * Micro OS — the commands of the shell that work with the files: the tree, the
 * content, the mode and the owner, and the file that becomes a program.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kshell_internal.h"

#include "kernel/config.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "libc/stdlib.h"
#include "libc/unistd.h"

#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
#include "fs_fat32/fat.h"
#define FSCK_USAGE "fsck [-y] [fat <point|device>]"
#define FSCK_DETAIL                                                                                \
    "Without -y the volume is only read: the report names the first problem and\n"                 \
    "nothing is repaired. With -y the damage the medium itself explains is put\n"                  \
    "right and the volume is checked again; what the medium cannot explain is\n"                   \
    "reported as (not repaired) for a person.\n"                                                   \
    "  -y, --repair        repair MicroFS: an orphan block goes back to the free\n"                \
    "                      map, an entry nobody can reach or that names a free\n"                  \
    "                      record is removed, a chain that loops or leaves the\n"                  \
    "                      volume is cut, the second of two objects that share a\n"                \
    "                      block gives it up, a size that does not match its chain\n"              \
    "                      is put right, and a record no name reaches is freed\n"                  \
    "  fat <point|device>  check the FAT volume at the point, or on the device;\n"                 \
    "                      -y repairs it the same way\n"
#else
#define FSCK_USAGE "fsck [-y]"
#define FSCK_DETAIL                                                                                \
    "Without -y the volume is only read: the report names the first problem and\n"                 \
    "nothing is repaired.\n"                                                                       \
    "  -y, --repair        repair what the medium itself explains: an orphan\n"                    \
    "                      block goes back to the free map, an entry nobody can\n"                 \
    "                      reach or that names a free record is removed, a chain\n"                \
    "                      that loops or leaves the volume is cut, a size that\n"                  \
    "                      does not match its chain is put right, and a record\n"                  \
    "                      no name reaches is freed. The volume is checked again\n"                \
    "                      afterwards, and what is left is (not repaired).\n"
#endif

#define SHELL_TREE_DEPTH 6
#define SHELL_PATH_MAX 128

/*
 * The value of PATH. It belongs to the shell and not to the filesystem or
 * the kernel: which directories hold the commands is a matter of the person
 * at the console, and a program of this system does not inherit it yet.
 */
#define SHELL_PATH_DEFAULT "/bin:/usr/bin"
#define SHELL_PATH_SIZE 96

static char g_shell_path[SHELL_PATH_SIZE] = SHELL_PATH_DEFAULT;

/* The nine bits of a mode as `ls -l` writes them; the body is below, with the
 * commands of the mode and the owner. */
static void mode_text(unsigned mode, char out[10]);

/* The path of the current directory of the task. */
static const char *shell_cwd(void)
{
    struct task *self = task_self();
    const char *cwd = (self != NULL) ? task_get_cwd(self) : NULL;

    if (cwd == NULL || cwd[0] == '\0') {
        return "/";
    }

    return cwd;
}

/*
 * Put a path together with the current directory, and normalize it: `.` is
 * the directory itself, `..` is the parent (the root stays the root), a run
 * of slashes is one slash and a trailing slash names the same entry. A path
 * that does not fit is refused: a path that is cut is a different path, and
 * a command that works with the cut path works with the wrong file.
 */
static bool path_add(char *out, k_size_t size, k_size_t *len, const char *name, k_size_t name_len)
{
    if (*len + 1 + name_len + 1 > size) {
        return false;
    }

    out[(*len)++] = '/';
    memcpy(out + *len, name, name_len);
    *len += name_len;
    out[*len] = '\0';

    return true;
}

static bool path_walk(char *out, k_size_t size, k_size_t *len, const char *text)
{
    const char *at = text;

    while (*at != '\0') {
        const char *start;
        k_size_t length;

        while (*at == '/') {
            at++;
        }
        start = at;
        while (*at != '\0' && *at != '/') {
            at++;
        }
        length = (k_size_t)(at - start);

        if (length == 0 || (length == 1 && start[0] == '.')) {
            continue;
        }

        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (*len > 0 && out[*len - 1] != '/') {
                (*len)--;
            }
            if (*len > 0) {
                (*len)--;
            }
            out[*len] = '\0';
            continue;
        }

        if (!path_add(out, size, len, start, length)) {
            return false;
        }
    }

    return true;
}

static bool shell_resolve(const char *path, char *out, k_size_t size)
{
    k_size_t len = 0;

    if (size < 2) {
        return false;
    }

    out[0] = '\0';

    /* A relative path starts at the directory of the task; an absolute one
     * starts at the root, and its own `..` never goes above it. */
    if (path == NULL || path[0] != '/') {
        if (!path_walk(out, size, &len, shell_cwd())) {
            return false;
        }
    }

    if (path != NULL && !path_walk(out, size, &len, path)) {
        return false;
    }

    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    out[len] = '\0';

    return true;
}

/* The same resolution for the runner of a pipeline: a redirection names its
 * file relative to the directory of the task, as every other path does. */
bool kshell_resolve_path(const char *path, char *out, k_size_t size)
{
    return shell_resolve(path, out, size);
}

/* The answer of a command that was given a path that does not fit. */
static int path_too_long(const char *command)
{
    kshell_printf("%s: path too long\n", command);

    return KSHELL_ERROR;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("%s\n", shell_cwd());

    return KSHELL_OK;
}

static int cmd_cd(int argc, char **argv)
{
    struct task *self = task_self();
    char path[SHELL_PATH_MAX];
    struct vfs_stat stat;

    if (argc < 2) {
        kshell_printf("usage: cd <directory>\n");
        return KSHELL_USAGE;
    }

    if (self == NULL) {
        return KSHELL_ERROR;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("cd");
    }

    if (vfs_stat(path, &stat) != 0) {
        kshell_printf("cd: %s: %s\n", path, vfs_strerror(-2));
        return KSHELL_ERROR;
    }

    if (stat.type != 2) {
        kshell_printf("cd: %s: not a directory\n", path);
        return KSHELL_ERROR;
    }

    task_set_cwd(self, path);

    return KSHELL_OK;
}

/* The path of a child of `dir`, when it fits; false when it does not. */
static bool ls_child_path(const char *dir, const char *name, char *out, k_size_t size)
{
    k_size_t dlen = strlen(dir);
    k_size_t nlen = strlen(name);
    bool slash = (dlen > 0 && dir[dlen - 1] != '/');

    if (dlen + (slash ? 1 : 0) + nlen + 1 > size) {
        return false;
    }

    memcpy(out, dir, dlen);
    if (slash) {
        out[dlen++] = '/';
    }
    memcpy(out + dlen, name, nlen + 1);

    return true;
}

/*
 * The directory above a path: the text of the path without its last entry.
 * The VFS does not resolve `..` inside a path, and the parent of `ls -a` does
 * not need it to: the path the shell holds is already absolute.
 */
static void path_parent(const char *path, char *out, k_size_t size)
{
    k_size_t len = strlen(path);

    if (size == 0) {
        return;
    }

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    while (len > 1 && path[len - 1] != '/') {
        len--;
    }
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    if (len >= size) {
        len = size - 1;
    }

    memcpy(out, path, len);
    out[len] = '\0';
}

/*
 * The entries of a directory, read in small pieces and printed as they
 * come. A fixed table cannot be the answer: a directory of 300 names has to
 * be listed whole, and the table of 64 entries the shell used to keep cut
 * the listing at 64 without a word. MicroFS reads its names through an open
 * descriptor (unbounded); the virtual filesystems of this system have a
 * listing and no descriptor, so they are read one name at a time. Both
 * paths are behind the same three calls.
 */
#define SHELL_DIR_CHUNK 4
#define SHELL_DIR_FALLBACK 64

struct shell_dir {
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_dirent chunk[SHELL_DIR_CHUNK];
    k_size_t at;
    k_size_t count;
    k_size_t fallback_index;
    bool stream;
    bool done;
    k_u8_t mount_seen;
};

/* The name of the mount at `point` when the point is a child of `parent`. */
static bool mount_child_name(const char *parent, const char *point, char *name, k_size_t size)
{
    const char *tail;
    k_size_t length;

    if (parent == NULL || point == NULL) {
        return false;
    }

    if (strcmp(parent, "/") == 0) {
        if (point[0] != '/') {
            return false;
        }
        tail = point + 1;
    } else {
        k_size_t plen = strlen(parent);

        if (strncmp(point, parent, plen) != 0 || point[plen] != '/') {
            return false;
        }
        tail = point + plen + 1;
    }

    length = strlen(tail);
    if (length == 0 || length > VFS_NAME_MAX || length + 1 > size || strchr(tail, '/') != NULL) {
        return false;
    }

    memcpy(name, tail, length + 1);

    return true;
}

/*
 * A mount point is an entry of the directory it hangs from even when the
 * filesystem of that directory knows nothing about it. The name is not
 * printed twice when the point is also a real entry of the directory.
 */
static void shell_dir_note_mount(struct shell_dir *dir, const char *path, const char *name)
{
    k_size_t count = vfs_mount_count();
    k_size_t i;

    for (i = 0; i < count && i < 8; i++) {
        char child[VFS_NAME_MAX + 1];

        if (mount_child_name(path, vfs_mount_name(i), child, sizeof(child)) &&
            strcmp(child, name) == 0) {
            dir->mount_seen |= (k_u8_t)(1u << i);
        }
    }
}

static bool shell_dir_next_mount(struct shell_dir *dir, const char *path, struct vfs_dirent *entry)
{
    k_size_t count = vfs_mount_count();
    k_size_t i;

    for (i = 0; i < count && i < 8; i++) {
        char child[VFS_NAME_MAX + 1];

        if ((dir->mount_seen & (k_u8_t)(1u << i)) != 0) {
            continue;
        }
        if (!mount_child_name(path, vfs_mount_name(i), child, sizeof(child))) {
            continue;
        }

        dir->mount_seen |= (k_u8_t)(1u << i);
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, child, strlen(child) + 1);
        entry->type = 2;

        return true;
    }

    return false;
}

/* The directory is opened as a directory; a listing without an open file
 * (procfs, devfs) is read through vfs_list on the first call. */
static void shell_dir_open(struct shell_dir *dir, const char *path)
{
    memset(dir, 0, sizeof(*dir));

    if (vfs_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, &dir->file, dir->priv) == 0) {
        dir->stream = true;
    }
}

/* 1 and an entry, 0 at the end, or the error of the filesystem. */
static int shell_dir_next(struct shell_dir *dir, const char *path, struct vfs_dirent *entry)
{
    if (!dir->done) {
        if (dir->stream) {
            for (;;) {
                int rc;

                if (dir->at < dir->count) {
                    *entry = dir->chunk[dir->at++];
                    shell_dir_note_mount(dir, path, entry->name);
                    return 1;
                }

                dir->at = 0;
                dir->count = 0;
                rc = vfs_readdir(&dir->file, dir->chunk, SHELL_DIR_CHUNK, &dir->count);
                if (rc != 0) {
                    dir->done = true;
                    return rc;
                }
                if (dir->count == 0) {
                    dir->done = true;
                    break;
                }
            }
        } else {
            /*
             * The static table is only borrowed for one name: the index is
             * the position, so a recursive walk of /proc can read its child
             * without losing the place of the parent.
             */
            static struct vfs_dirent list[SHELL_DIR_FALLBACK];
            k_size_t count = 0;
            int rc = vfs_list(path, list, SHELL_DIR_FALLBACK, &count);

            if (rc != 0) {
                dir->done = true;
                return rc;
            }
            if (dir->fallback_index >= count) {
                dir->done = true;
            } else {
                *entry = list[dir->fallback_index++];
                shell_dir_note_mount(dir, path, entry->name);
                return 1;
            }
        }
    }

    return shell_dir_next_mount(dir, path, entry) ? 1 : 0;
}

static void shell_dir_close(struct shell_dir *dir)
{
    if (dir->stream) {
        (void)vfs_close(&dir->file);
        dir->stream = false;
    }
}

/*
 * The colour of a name for `ls --color`: blue for a directory and green for
 * a file with an execute bit, which are the two colours a person expects of
 * a listing. The colours are the SGR sequences of a terminal (kernel/ansi.h),
 * so the screen shows them and the serial copy does not carry them (the port
 * strips them); a name of no such kind is written plain.
 */
#define LS_COLOR_DIR "\033[34m"
#define LS_COLOR_EXEC "\033[32m"
#define LS_COLOR_OFF "\033[0m"

static const char *ls_color_start(k_size_t type, bool executable)
{
    if (type == 2) {
        return LS_COLOR_DIR;
    }
    if (executable) {
        return LS_COLOR_EXEC;
    }

    return "";
}

/* Is the execute bit of a mode of this system set? The nine bits are the
 * POSIX ones, which the call answers. */
static bool ls_executable(const struct vfs_statx *info)
{
    return info->type == 1 && (info->mode & 0111u) != 0u;
}

/* The name of one entry, in the colours of `--color` when the flag asked for
 * them. The long format colours the name and not the line: the fields are a
 * record and a colour of the mode would read as a statement about it. */
static void ls_put_name(const char *name, k_size_t type, bool executable, bool color)
{
    const char *start = color ? ls_color_start(type, executable) : "";

    if (start[0] == '\0') {
        kshell_printf("%s%s\n", name, type == 2 ? "/" : "");
        return;
    }

    kshell_printf("%s%s%s%s\n", start, name, type == 2 ? "/" : "", LS_COLOR_OFF);
}

/*
 * The moment of a record as `ls -l` writes it: `DD Mon HH:MM`, in UTC, which
 * is the time the filesystem keeps (ADR 0013). A record without a moment is
 * a dash; nothing is pretended about it.
 */
static void ls_date(k_u64_t when, char out[13])
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const k_u64_t last_second = 253402300799ULL; /* 9999-12-31 23:59:59 */
    long long days;
    long long z;
    long long era;
    unsigned doe;
    unsigned yoe;
    unsigned doy;
    unsigned mp;
    unsigned day;
    unsigned month;
    unsigned seconds;

    if (when == 0 || when > last_second) {
        snprintf(out, 13, "-");
        return;
    }

    days = (long long)(when / 86400u);
    seconds = (unsigned)(when % 86400u);

    /* The civil date of a count of days since 1970-01-01, in whole years. */
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;

    snprintf(out, 13, "%02u %c%c%c %02u:%02u", day, months[(month - 1) * 3],
             months[(month - 1) * 3 + 1], months[(month - 1) * 3 + 2], seconds / 3600,
             (seconds / 60) % 60);
}

/*
 * One line of `ls -l`: the kind and the nine bits of the mode, the owner, the
 * size, the moment and the name, in fixed columns. A record of an old volume
 * keeps no mode: the `?` says exactly that, because nine dashes would read as
 * "nobody may touch it", which the record does not say.
 */
static void ls_long_stat(const struct vfs_statx *info, const char *name, bool slash,
                         bool executable, bool color)
{
    char bits[10];
    char mode[11];
    char owner[12];
    char date[13];
    const char *start = color ? ls_color_start(info->type, executable) : "";
    const char *reset = start[0] != '\0' ? LS_COLOR_OFF : "";

    if (info->mode == 0) {
        snprintf(mode, sizeof(mode), "?---------");
    } else {
        char kind = (info->type == 2) ? 'd' : (info->type == 1 ? '-' : 'c');

        mode_text(info->mode & KSYS_MODE_MASK, bits);
        snprintf(mode, sizeof(mode), "%c%s", kind, bits);
    }

    if (info->owner == 0) {
        snprintf(owner, sizeof(owner), "root");
    } else {
        snprintf(owner, sizeof(owner), "%u", (unsigned)info->owner);
    }

    ls_date(info->mtime, date);
    kshell_printf("%s %-7s %6llu %12s %s%s%s%s\n", mode, owner, (unsigned long long)info->size,
                  date, start, name, slash ? "/" : "", reset);
}

/* One entry of a directory in the long format. The stat is of the path of
 * the entry and not of its name, which would find another file. */
static void ls_long_entry(const char *dir, const struct vfs_dirent *entry, bool color)
{
    char child[SHELL_PATH_MAX];
    struct vfs_statx info;

    if (!ls_child_path(dir, entry->name, child, sizeof(child)) || vfs_statx(child, &info) != 0) {
        /* The name is still the answer of the listing; the fields that
         * could not be read are not invented for it. */
        kshell_printf("?--------- %-7s %6s %12s %s%s\n", "-", "-", "-", entry->name,
                      entry->type == 2 ? "/" : "");
        return;
    }

    ls_long_stat(&info, entry->name, entry->type == 2, ls_executable(&info), color);
}

/*
 * The words of the line without the `--color` options, and what the flag
 * asked for. The helper of the options knows groups of short letters and
 * `--help`, not a long option with a value, so the flag of the colours is
 * taken here and the rest of the line goes to kshell_options().
 */
#define LS_COLOR_NEVER 0
#define LS_COLOR_ALWAYS 1
#define LS_COLOR_AUTO 2

static int ls_take_color(int argc, char **argv, char **rest, int *rest_argc)
{
    int color = LS_COLOR_NEVER;
    int written = 0;
    int i;

    for (i = 0; i < argc; i++) {
        const char *word = argv[i];

        if (i > 0 && strncmp(word, "--color", 7) == 0) {
            const char *when = word + 7;

            if (when[0] == '\0') {
                color = LS_COLOR_ALWAYS; /* --color alone is --color=always */
                continue;
            }
            if (when[0] == '=' && strcmp(when + 1, "always") == 0) {
                color = LS_COLOR_ALWAYS;
                continue;
            }
            if (when[0] == '=' && strcmp(when + 1, "never") == 0) {
                color = LS_COLOR_NEVER;
                continue;
            }
            if (when[0] == '=' && strcmp(when + 1, "auto") == 0) {
                color = LS_COLOR_AUTO;
                continue;
            }
            if (when[0] == '=') {
                kshell_printf("ls: %s: the flag is always, never or auto\n", word);
                return -1;
            }
            /* Another long option of that prefix: the caller refuses it the
             * way it refuses every option it does not know. */
        }

        rest[written++] = argv[i];
    }

    rest[written] = NULL;
    *rest_argc = written;

    /*
     * `auto` is off: this shell has no terminal to ask about - the console of
     * the machine is a serial port and a screen at once, and a program that
     * reads the words of a pipe would get the sequences as text. A person who
     * wants colours says `--color` or `--color=always`.
     */
    return color == LS_COLOR_ALWAYS ? 1 : 0;
}

static int cmd_ls(int argc, char **argv)
{
    /*
     * The directory is read one entry at a time through the iterator, so a
     * listing does not need a table of its names on the stack of a task; the
     * words of the line are the ones without the `--color` flags.
     */
    struct shell_dir dir;
    struct vfs_dirent entry;
    char *words[KSHELL_MAX_ARGS + 2];
    bool long_format = false;
    bool show_all = false;
    bool color;
    struct kshell_option options[] = {
        {'l', &long_format},
        {'a', &show_all},
    };
    struct kshell_args args;
    struct vfs_stat info;
    char path[SHELL_PATH_MAX];
    const char *shown;
    int rc;
    int colors;
    int words_count = 0;

    if (argc > KSHELL_MAX_ARGS + 2) {
        kshell_printf("ls: too many arguments\n");
        return KSHELL_USAGE;
    }

    colors = ls_take_color(argc, argv, words, &words_count);
    if (colors < 0) {
        kshell_printf("usage: ls [-la] [--color[=always|never|auto]] [path]\n");
        return KSHELL_USAGE;
    }
    color = colors == 1;

    rc = kshell_options(words_count, words, options, K_ARRAY_SIZE(options),
                        "ls [-la] [--color[=always|never|auto]] [path]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc > 1) {
        kshell_printf("usage: ls [-la] [--color[=always|never|auto]] [path]\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(args.argc > 0 ? args.argv[0] : NULL, path, sizeof(path))) {
        return path_too_long("ls");
    }

    rc = vfs_stat(path, &info);
    if (rc != 0) {
        kshell_printf("ls: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    /* The name a person typed, not the path the shell made of it: `ls hello`
     * answers `hello`, exactly as the argument was given. */
    shown = args.argc > 0 ? args.argv[0] : path;

    /* A file has no entries below it: plain `ls` prints its name, and `-l`
     * prints the one line of stat for it. */
    if (info.type != 2) {
        if (long_format) {
            struct vfs_statx info_x;

            rc = vfs_statx(path, &info_x);
            if (rc != 0) {
                kshell_printf("ls: %s: %s\n", path, vfs_strerror(rc));
                return KSHELL_ERROR;
            }
            ls_long_stat(&info_x, shown, false, ls_executable(&info_x), color);
        } else {
            kshell_printf("%s\n", shown);
        }
        return KSHELL_OK;
    }

    shell_dir_open(&dir, path);

    if (long_format && show_all) {
        char parent[SHELL_PATH_MAX];
        struct vfs_statx self_info;
        struct vfs_statx parent_info;

        if (vfs_statx(path, &self_info) == 0) {
            ls_long_stat(&self_info, ".", false, false, color);
        }
        path_parent(path, parent, sizeof(parent));
        /* A parent that cannot be read is not invented: the row is
         * missing rather than wrong. */
        if (vfs_statx(parent, &parent_info) == 0) {
            ls_long_stat(&parent_info, "..", false, false, color);
        }
    }

    if (show_all && !long_format) {
        /*
         * `.` and `..` are not entries of a directory and no filesystem here
         * lists them: they are the two names every directory has.
         */
        kshell_printf(".\n..\n");
    }

    while ((rc = shell_dir_next(&dir, path, &entry)) > 0) {
        if (long_format) {
            ls_long_entry(path, &entry, color);
            continue;
        }

        if (color) {
            char child[SHELL_PATH_MAX];
            struct vfs_statx child_info;
            bool executable = false;

            /* The mode is one stat per name, and a listing of colours is the
             * command of a person at the console, not of a script. */
            if (entry.type == 1 && ls_child_path(path, entry.name, child, sizeof(child)) &&
                vfs_statx(child, &child_info) == 0) {
                executable = ls_executable(&child_info);
            }

            ls_put_name(entry.name, entry.type, executable, true);
        } else {
            ls_put_name(entry.name, entry.type, false, false);
        }
    }

    shell_dir_close(&dir);

    if (rc < 0) {
        kshell_printf("ls: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/*
 * One directory of the tree, read through the same iterator as `ls`. The
 * state of a level is apart, because the walk of a child runs before the
 * parent is done with its own names; the buffer is the one entry of the
 * parent and the one of the child, never a table of the directory.
 *
 * The sixth level is the last one drawn: a directory that has entries below
 * it says `... (deeper)` instead of running the stack of the shell out. The
 * entries of that level are still counted, so the summary is the content of
 * the tree and not the branches that were drawn.
 */
static bool tree_next(struct shell_dir *dir, const char *path, bool dirs_only,
                      struct vfs_dirent *entry, k_size_t *files, int *rc)
{
    int status;

    while ((status = shell_dir_next(dir, path, entry)) > 0) {
        if (dirs_only && entry->type != 2) {
            (*files)++;
            continue;
        }

        *rc = 0;
        return true;
    }

    *rc = status;

    return false;
}

static int tree_walk(const char *path, const char *prefix, int level, bool dirs_only,
                     k_size_t *dirs, k_size_t *files)
{
    static struct shell_dir state[SHELL_TREE_DEPTH];
    struct shell_dir *dir;
    struct vfs_dirent entries[2];
    struct vfs_dirent *current;
    struct vfs_dirent *following;
    bool have;
    int rc = 0;

    if (level < 1 || level > SHELL_TREE_DEPTH) {
        return 0;
    }

    dir = &state[level - 1];
    shell_dir_open(dir, path);

    if (level == SHELL_TREE_DEPTH) {
        bool printable = false;
        int status;

        while ((status = shell_dir_next(dir, path, &entries[0])) > 0) {
            if (entries[0].type == 2) {
                (*dirs)++;
                printable = true;
            } else {
                (*files)++;
                if (!dirs_only) {
                    printable = true;
                }
            }
        }

        shell_dir_close(dir);

        if (status < 0) {
            return status;
        }
        if (printable) {
            kshell_printf("%s... (deeper)\n", prefix);
        }

        return 0;
    }

    current = &entries[0];
    following = &entries[1];
    have = tree_next(dir, path, dirs_only, current, files, &rc);

    while (have) {
        bool have_next;
        bool last;
        char child[SHELL_PATH_MAX];
        char child_prefix[SHELL_PATH_MAX];

        have_next = tree_next(dir, path, dirs_only, following, files, &rc);
        last = !have_next;

        kshell_printf("%s%s %s%s\n", prefix, last ? "`--" : "|--", current->name,
                      current->type == 2 ? "/" : "");

        if (current->type == 2) {
            (*dirs)++;

            if (ls_child_path(path, current->name, child, sizeof(child))) {
                snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix,
                         last ? "    " : "|   ");
                (void)tree_walk(child, child_prefix, level + 1, dirs_only, dirs, files);
            }
        } else {
            (*files)++;
        }

        {
            struct vfs_dirent *swap = current;

            current = following;
            following = swap;
        }
        have = have_next;
    }

    shell_dir_close(dir);

    return rc;
}

static int cmd_tree(int argc, char **argv)
{
    bool show_all = false;
    bool dirs_only = false;
    struct kshell_option options[] = {
        {'a', &show_all},
        {'d', &dirs_only},
    };
    struct kshell_args args;
    struct vfs_stat info;
    char path[SHELL_PATH_MAX];
    k_size_t dirs = 0;
    k_size_t files = 0;
    int rc;

    rc = kshell_options(argc, argv, options, K_ARRAY_SIZE(options), "tree [-ad] [path]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc > 1) {
        kshell_printf("usage: tree [-ad] [path]\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(args.argc > 0 ? args.argv[0] : NULL, path, sizeof(path))) {
        return path_too_long("tree");
    }

    rc = vfs_stat(path, &info);
    if (rc != 0) {
        kshell_printf("tree: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("%s\n", path);

    if (info.type == 2) {
        rc = tree_walk(path, "", 1, dirs_only, &dirs, &files);
        if (rc != 0) {
            kshell_printf("tree: %s: %s\n", path, vfs_strerror(rc));
            return KSHELL_ERROR;
        }
    } else {
        files = 1;
    }

    /* `-a` is accepted for the hidden names; no filesystem here lists a
     * name of that kind yet, and `.` and `..` would be a branch nobody has. */
    (void)show_all;

    kshell_printf("%u %s, %u %s\n", (unsigned)dirs, dirs == 1 ? "directory" : "directories",
                  (unsigned)files, files == 1 ? "file" : "files");

    return KSHELL_OK;
}

/*
 * The bytes of the file with the number of the line in front of every line.
 * The state of the numbering belongs to the caller: a line that spans two
 * reads of 128 bytes is numbered once, at its first byte.
 */
static void cat_numbered(const char *buffer, long len, unsigned *line, bool *at_start)
{
    long i = 0;

    while (i < len) {
        long start = i;

        while (i < len && buffer[i] != '\n') {
            i++;
        }

        if (*at_start) {
            kshell_printf("%6u\t", *line);
            (*line)++;
            *at_start = false;
        }

        if (i > start) {
            kshell_printf("%.*s", (int)(i - start), buffer + start);
        }

        if (i < len) {
            /* The newline itself closes the line; the next one begins after
             * it, even when it is empty. */
            kshell_printf("\n");
            i++;
            *at_start = true;
        }
    }
}

/* Copy a descriptor to the output of the shell until the end of the file. */
static int cat_copy(int fd, const char *label, bool number)
{
    char buffer[128];
    unsigned line = 1;
    bool at_start = true;

    for (;;) {
        long got = microos_read(fd, buffer, sizeof(buffer));

        if (got == 0) {
            return KSHELL_OK; /* the end of the file, or the last writer gone */
        }

        if (got < 0) {
            /*
             * A pipe gives up after half a second with a producer that is
             * still there (ADR 0014): that is not the end of the file, and
             * the command of a pipeline has to wait it out.
             */
            if (microos_errno() == -KERR_AGAIN) {
                (void)microos_yield();
                continue;
            }

            kshell_eprintf("cat: %s: %s\n", label, microos_strerror(microos_errno()));
            return KSHELL_ERROR;
        }

        if (number) {
            cat_numbered(buffer, got, &line, &at_start);
        } else {
            kshell_write(buffer, (k_size_t)got);
        }
    }
}

static int cmd_cat(int argc, char **argv)
{
    bool number = false;
    struct kshell_option options[] = {
        {'n', &number},
    };
    struct kshell_args args;
    char path[SHELL_PATH_MAX];
    int rc;
    int fd;

    rc = kshell_options(argc, argv, options, K_ARRAY_SIZE(options), "cat [-n] [file]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc > 1) {
        kshell_printf("usage: cat [-n] [file]\n");
        return KSHELL_USAGE;
    }

    /*
     * Without a name `cat` is the second stage of a pipe or a `cat < file`:
     * it copies its standard input, which is where the console used to be
     * absent. A descriptor 0 that no line opened is an error of the call and
     * is said, not invented as an empty file.
     */
    if (args.argc == 0) {
        return cat_copy(0, "(stdin)", number);
    }

    if (!shell_resolve(args.argv[0], path, sizeof(path))) {
        return path_too_long("cat");
    }

    /*
     * The file is opened through the library of the system, that is, through
     * the calls of the kernel. The shell and the programs of the user see
     * the same files.
     */
    fd = microos_open(path, MICROOS_O_RDONLY);
    if (fd < 0) {
        kshell_eprintf("cat: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    rc = cat_copy(fd, path, number);

    if (microos_close(fd) != 0 && rc == KSHELL_OK) {
        kshell_eprintf("cat: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    return rc;
}

/* A count of bytes with the word a person reads: one byte, two bytes. */
static const char *byte_word(k_size_t count)
{
    return count == 1 ? "byte" : "bytes";
}

static int cmd_write(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    char text[128];
    k_size_t len = 0;
    bool cut = false;
    int fd;
    int i;

    if (argc < 3) {
        kshell_printf("usage: write <file> <text>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("write");
    }

    /*
     * The words are copied by hand: snprintf gives the length it would
     * have written, and a length like that would push the next character
     * out of the buffer.
     */
    for (i = 2; i < argc && len < sizeof(text) - 1; i++) {
        k_size_t room = sizeof(text) - len;
        k_size_t word = strlen(argv[i]);

        if (len > 0) {
            text[len++] = ' ';
            room = sizeof(text) - len;
        }

        if (word > room - 1) {
            word = room - 1;
            cut = true;
        }

        memcpy(text + len, argv[i], word);
        len += word;
    }
    text[len] = '\0';

    /* The file is written through the library, that is, through the calls. */
    fd = microos_open(path, MICROOS_O_WRONLY | MICROOS_O_CREAT | MICROOS_O_TRUNC);
    if (fd < 0) {
        kshell_printf("write: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    if (cut) {
        kshell_printf("write: the text does not fit, it is cut at %u %s\n", (unsigned)len,
                      byte_word(len));
    }

    /* Every call is checked: a write that failed must not be reported. */
    {
        long written = microos_write(fd, text, len);
        long newline = microos_write(fd, "\n", 1);

        if (written != (long)len || newline != 1) {
            kshell_printf("write: %s: %s\n", path, microos_strerror(microos_errno()));
            microos_close(fd);
            return KSHELL_ERROR;
        }
    }

    if (microos_close(fd) != 0) {
        kshell_printf("write: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    kshell_printf("%u %s written to %s\n", (unsigned)(len + 1), byte_word(len + 1), path);

    return KSHELL_OK;
}

static int cmd_mkdir(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    int rc;

    if (argc < 2) {
        kshell_printf("usage: mkdir <directory>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("mkdir");
    }

    rc = vfs_mkdir(path);
    if (rc != 0) {
        /*
         * A directory that is already there is not a file that is in the
         * way: the sentence of POSIX "file exists" would send a person
         * looking for a file. The name is asked what it is, and a directory
         * says so.
         */
        if (rc == KERR_EXIST) {
            struct vfs_stat info;

            if (vfs_stat(path, &info) == 0 && info.type == 2) {
                kshell_printf("mkdir: %s: is a directory\n", path);
                return KSHELL_ERROR;
            }
        }

        kshell_printf("mkdir: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/*
 * The deepest tree `rm -r` walks. The stack of the shell task is 8 KB and a
 * path is 128 bytes; a tree deeper than this is refused with a sentence
 * instead of running the stack out in the middle of a removal.
 */
#define SHELL_RM_DEPTH 8

/* The names of one pass over a directory. The walk below uses the same static
 * table for every directory, so only the first entry is taken at a time: the
 * listing is read again after each removal, and the loop ends on the empty
 * answer of the filesystem and not on a guess about the size of the tree. */
#define SHELL_RM_ENTRIES 16

static int rm_path(const char *path, bool recursive, int depth);

/* Remove every entry of a directory that is already known to be a directory. */
static int rm_children(const char *path, int depth)
{
    static struct vfs_dirent entries[SHELL_RM_ENTRIES];
    char name[VFS_NAME_MAX + 1];
    char child[SHELL_PATH_MAX];

    for (;;) {
        k_size_t count = 0;
        int rc = vfs_list(path, entries, SHELL_RM_ENTRIES, &count);

        if (rc != 0) {
            kshell_printf("rm: %s: %s\n", path, vfs_strerror(rc));
            return KSHELL_ERROR;
        }

        if (count == 0) {
            return KSHELL_OK;
        }

        /* The name is copied before the walk goes one level down: the table
         * belongs to the directory that was listed last. */
        snprintf(name, sizeof(name), "%s", entries[0].name);

        if (!ls_child_path(path, name, child, sizeof(child))) {
            return path_too_long("rm");
        }

        if (rm_path(child, true, depth + 1) != KSHELL_OK) {
            return KSHELL_ERROR;
        }
    }
}

/*
 * Remove one object. A directory goes away only with `-r`, and a directory
 * that is not empty goes away only after its entries: the filesystem refuses
 * to drop a record that still holds names, which is what the check of the
 * non-empty directory is.
 */
static int rm_path(const char *path, bool recursive, int depth)
{
    struct vfs_stat info;
    int rc;

    rc = vfs_stat(path, &info);
    if (rc != 0) {
        kshell_printf("rm: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    if (info.type == 2) {
        if (!recursive) {
            kshell_printf("rm: %s: is a directory, use -r\n", path);
            return KSHELL_ERROR;
        }

        if (depth >= SHELL_RM_DEPTH) {
            kshell_printf("rm: %s: the tree is too deep\n", path);
            return KSHELL_ERROR;
        }

        if (rm_children(path, depth) != KSHELL_OK) {
            return KSHELL_ERROR;
        }
    }

    rc = vfs_unlink(path);
    if (rc != 0) {
        kshell_printf("rm: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int cmd_rm(int argc, char **argv)
{
    bool recursive = false;
    struct kshell_option options[] = {
        {'r', &recursive},
    };
    struct kshell_args args;
    char path[SHELL_PATH_MAX];
    int rc;

    rc = kshell_options(argc, argv, options, K_ARRAY_SIZE(options), "rm [-r] <path>", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc != 1) {
        kshell_printf("usage: rm [-r] <path>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(args.argv[0], path, sizeof(path))) {
        return path_too_long("rm");
    }

    return rm_path(path, recursive, 0);
}

/* A number of bytes for a person: whole kilobytes below a megabyte, one
 * decimal of a megabyte above it. */
static void df_human(k_u64_t bytes, char *out, k_size_t size)
{
    const k_u64_t mb = 1024u * 1024u;

    if (bytes >= mb) {
        (void)snprintf(out, size, "%u.%u MB", (unsigned)(bytes / mb),
                       (unsigned)((bytes % mb) * 10u / mb));
    } else if (bytes >= 1024u) {
        (void)snprintf(out, size, "%u KB", (unsigned)(bytes / 1024u));
    } else {
        (void)snprintf(out, size, "%u bytes", (unsigned)bytes);
    }
}

static int cmd_df(int argc, char **argv)
{
    bool human = false;
    struct kshell_option options[] = {
        {'h', &human},
    };
    struct vfs_statfs stat;
    struct vfs_stat info;
    struct kshell_args args;
    char path[SHELL_PATH_MAX];
    int rc;

    rc = kshell_options(argc, argv, options, K_ARRAY_SIZE(options), "df [-h] [path]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc > 1) {
        kshell_printf("usage: df [-h] [path]\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(args.argc > 0 ? args.argv[0] : NULL, path, sizeof(path))) {
        return path_too_long("df");
    }

    /*
     * A path that is not there is not the volume of the root: the numbers
     * of a volume belong to a path, and `df /nope` answers the error of the
     * name before it reads any number.
     */
    rc = vfs_stat(path, &info);
    if (rc != 0) {
        kshell_printf("df: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    rc = vfs_statfs(path, &stat);
    if (rc != 0) {
        kshell_printf("df: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    if (human) {
        char total[24];
        char free_bytes[24];

        /* The blocks of the volume are its own unit; a person reads the
         * bytes, and that is all `-h` changes. */
        df_human(stat.total_blocks * (k_u64_t)stat.block_size, total, sizeof(total));
        df_human(stat.free_blocks * (k_u64_t)stat.block_size, free_bytes, sizeof(free_bytes));

        kshell_printf("%s: %s total, %s free\n", path, total, free_bytes);
        kshell_printf("inodes: %u of %u free\n", (unsigned)stat.free_inodes,
                      (unsigned)stat.total_inodes);

        return KSHELL_OK;
    }

    kshell_printf("blocks: %u of %u free (%u KB each)\n", (unsigned)stat.free_blocks,
                  (unsigned)stat.total_blocks, (unsigned)(stat.block_size / 1024));
    kshell_printf("inodes: %u of %u free\n", (unsigned)stat.free_inodes,
                  (unsigned)stat.total_inodes);

    return KSHELL_OK;
}

/* The filesystems of the tree: the list, the mount and the unmount. */

/*
 * Tell a person why a mount or an unmount was refused. The text of a code
 * is the text of the VFS (and of the filesystem that knows the code), not
 * the "unknown error" of a library that was never told about EBUSY.
 */
static int mount_error_text(const char *what, const char *name, int code)
{
    kshell_printf("%s: %s: %s\n", what, name, vfs_strerror(code));

    return KSHELL_ERROR;
}

static int cmd_mount(int argc, char **argv)
{
    struct kshell_args args;
    bool all = false;
    struct kshell_option options[1];
    k_size_t count;
    k_size_t i;
    int rc;

    options[0].letter = 'a';
    options[0].value = &all;

    rc = kshell_options(argc, argv, options, 1, "mount [-a] [<device> <point> <type>]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (all) {
        if (args.argc != 0) {
            kshell_printf("usage: mount -a\n");
            return KSHELL_USAGE;
        }

        rc = vfs_mount_fstab(VFS_FSTAB_PATH);
        if (rc < 0) {
            return mount_error_text("mount", VFS_FSTAB_PATH, rc);
        }

        kshell_printf("mount: %d filesystem(s) mounted from %s\n", rc, VFS_FSTAB_PATH);

        return KSHELL_OK;
    }

    if (args.argc == 0) {
        count = vfs_mount_count();

        if (count == 0) {
            kshell_printf("no filesystem is mounted\n");
            return KSHELL_OK;
        }

        for (i = 0; i < count; i++) {
            kshell_printf("%s\n", vfs_mount_name(i));
        }

        return KSHELL_OK;
    }

    if (args.argc != 3) {
        kshell_printf("usage: mount [<device> <point> <type>] | mount -a\n");
        return KSHELL_USAGE;
    }

    /*
     * The words are `device point type`, as in the table of /etc/fstab and
     * in the mount of Linux; the call of the VFS takes the point first.
     */
    if (vfs_type_find(args.argv[2]) < 0) {
        kshell_printf("mount: there is no filesystem type '%s'\n", args.argv[2]);
        return KSHELL_ERROR;
    }

    rc = vfs_mount_type(args.argv[1], args.argv[2], args.argv[0]);
    if (rc != 0) {
        return mount_error_text("mount", args.argv[1], rc);
    }

    kshell_printf("mount: %s is mounted at %s\n", args.argv[0], args.argv[1]);

    return KSHELL_OK;
}

static int cmd_umount(int argc, char **argv)
{
    struct kshell_args args;
    char point[SHELL_PATH_MAX];
    int rc;

    rc = kshell_options(argc, argv, NULL, 0, "umount <point>", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc != 1) {
        kshell_printf("usage: umount <point>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(args.argv[0], point, sizeof(point))) {
        return path_too_long("umount");
    }

    /*
     * The root of the tree stays. A task that takes it away leaves every
     * path of the machine without a filesystem, and the console cannot even
     * read the program that would put it back. The volume is formatted by
     * `mkfs -y` where the machine has it, and by the image of a machine
     * that does not; this command never goes through either.
     */
    if (strcmp(point, "/") == 0) {
        kshell_printf("umount: / is the root of the tree; it stays. The volume is formatted by\n"
                      "mkfs -y, or by the image of a machine that has no mkfs.\n");
        return KSHELL_ERROR;
    }

    /*
     * /proc and /dev are mounted by the boot and no type of the table can
     * mount them again: an unmount would leave the machine without its
     * information and its devices until the next reboot. A machine that does
     * not mount one of them is left to the ordinary answer of the VFS.
     */
    if (strcmp(point, "/proc") == 0 || strcmp(point, "/dev") == 0) {
        k_size_t count = vfs_mount_count();
        k_size_t i;

        for (i = 0; i < count; i++) {
            if (strcmp(vfs_mount_name(i), point) == 0) {
                kshell_printf("umount: %s is mounted by the kernel and stays\n", point);
                return KSHELL_ERROR;
            }
        }
    }

    rc = vfs_umount(point);
    if (rc != 0) {
        return mount_error_text("umount", point, rc);
    }

    kshell_printf("umount: %s is out of the tree\n", point);

    return KSHELL_OK;
}

/* --- the mode, the owner, the path and the programs --- */

/*
 * A number of a command line, in the base the command speaks. The whole text
 * has to be the number: "0644x" is not a mode that was cut to 0644, and a
 * number above the limit is refused instead of wrapped.
 */
static bool shell_number(const char *text, int base, unsigned long limit, unsigned long *out)
{
    char *end = NULL;
    unsigned long value = microos_strtoul(text, &end, base);

    if (end == NULL || end == text || *end != '\0' || value > limit) {
        return false;
    }

    *out = value;

    return true;
}

/*
 * The words of the shell for a call that refused a mode or an owner. The
 * library gives the number of POSIX; the person at the console wants a
 * sentence, and the invalid argument needs its own, because the number does
 * not say which argument was wrong.
 */
static const char *attr_error_text(int code, const char *invalid)
{
    switch (code) {
    case -KERR_PERM:
        return "not permitted";
    case -KERR_NOENT:
        /* The POSIX sentence, the same one every command of the shell says
         * for a name that is not there. */
        return "no such file or directory";
    case -KERR_INVAL:
        return invalid;
    default:
        return microos_strerror(code);
    }
}

#if KERNEL_EXEC
/*
 * The words for a program that did not run. Every reason of the call has a
 * face of its own: "function not implemented" would hide that this machine
 * has no window of programs, which is the answer of the board.
 */
static const char *exec_error_text(int code)
{
    if (code == -KERR_NOSYS) {
        return "this machine cannot run a program";
    }

    if (code == -KERR_PERM) {
        return "not permitted: the file has no execute bit";
    }

    if (code == -KERR_NOMEM) {
        return "not enough memory for the image";
    }

    return attr_error_text(code, "not a MOX1 image of this system");
}
#endif /* KERNEL_EXEC */

static int cmd_chmod(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    unsigned long mode = 0;
    int rc;

    if (argc != 3) {
        kshell_printf("usage: chmod <mode> <path>\n");
        return KSHELL_USAGE;
    }

    if (!shell_number(argv[1], 8, KSYS_MODE_MASK, &mode)) {
        kshell_printf("chmod: %s: not an octal mode (000..777)\n", argv[1]);
        return KSHELL_ERROR;
    }

    if (!shell_resolve(argv[2], path, sizeof(path))) {
        return path_too_long("chmod");
    }

    rc = microos_chmod(path, (unsigned)mode);
    if (rc != 0) {
        kshell_printf("chmod: %s: %s\n", path,
                      attr_error_text(microos_errno(), "000 is not a mode the filesystem keeps"));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int cmd_chown(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    unsigned long owner = 0;
    int rc;

    if (argc != 3) {
        kshell_printf("usage: chown <uid> <path>\n");
        return KSHELL_USAGE;
    }

    if (!shell_number(argv[1], 10, KSYS_UID_MAX, &owner)) {
        kshell_printf("chown: %s: a uid is 0..%u\n", argv[1], (unsigned)KSYS_UID_MAX);
        return KSHELL_ERROR;
    }

    if (!shell_resolve(argv[2], path, sizeof(path))) {
        return path_too_long("chown");
    }

    rc = microos_chown(path, (unsigned)owner);
    if (rc != 0) {
        kshell_printf("chown: %s: %s\n", path,
                      attr_error_text(microos_errno(), "a uid is 0..32767"));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/* The nine bits of a mode as ls writes them: a live bit is a letter. */
static void mode_text(unsigned mode, char out[10])
{
    static const char letters[3] = {'r', 'w', 'x'};
    int bit;

    for (bit = 0; bit < 9; bit++) {
        out[bit] = (mode & (1u << (8 - bit))) ? letters[bit % 3] : '-';
    }
    out[9] = '\0';
}

/*
 * The mode, the owner and the size of one object. The line of `ls -l` comes
 * first because it is the one a person reads at a glance; the fields below
 * it are the same numbers without the puzzle of the nine bits.
 *
 * A record of an old volume keeps no mode at all. Nine dashes would be read
 * as "nobody may touch it", which is a different and false statement, so
 * such an object prints the fields alone and says `mode: (not set)`.
 */
static int cmd_stat(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    struct vfs_stat info;
    unsigned mode;
    const char *type;
    int rc;

    if (argc != 2) {
        kshell_printf("usage: stat <path>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("stat");
    }

    rc = vfs_stat(path, &info);
    if (rc != 0) {
        kshell_printf("stat: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    mode = info.mode & KSYS_MODE_MASK;

    if (info.type == 2) {
        type = "directory";
    } else if (info.type == 1) {
        type = "file";
    } else {
        type = "device";
    }

    if (mode != 0) {
        char bits[10];
        char kind = (info.type == 2) ? 'd' : (info.type == 1 ? '-' : 'c');

        mode_text(mode, bits);
        kshell_printf("%c%s %u %llu %s\n", kind, bits, (unsigned)info.owner,
                      (unsigned long long)info.size, path);
    }

    kshell_printf("type: %s\n", type);

    if (mode == 0) {
        kshell_printf("mode: (not set)\n");
    } else {
        kshell_printf("mode: 0%o\n", mode);
    }

    kshell_printf("owner: %u\n", (unsigned)info.owner);
    kshell_printf("size: %llu\n", (unsigned long long)info.size);

    return KSHELL_OK;
}

/*
 * The consistency of the volume. Without `-y` the filesystem answers with
 * the report and not with a repair: a person decides what to do about a
 * broken record, and a checker that writes is a formatter that nobody
 * asked for. With `-y` the safe repairs are applied and the volume is
 * checked again, so the answer is about the medium and not about the plan;
 * the problems the pass refused are named as left.
 */
#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
static int fsck_fat(const char *what, bool write)
{
    struct fat_check_report report;
    int rc;

    rc = fat_repair(what, &report, write);
    if (rc == KERR_NOENT) {
        kshell_printf("fsck: %s: no such device\n", what);
        return KSHELL_ERROR;
    }

    if (rc == KERR_INVAL) {
        kshell_printf("fsck: %s: not a FAT volume of this machine\n", what);
        return KSHELL_ERROR;
    }

    if (rc != 0) {
        kshell_printf("fsck: %s: %s\n", what, fat_strerror(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("fsck: FAT%u, %u file(s), %u directory(ies), %u of %u cluster(s) free\n",
                  (unsigned)report.type, (unsigned)report.files, (unsigned)report.directories,
                  (unsigned)report.free_clusters, (unsigned)report.clusters);

    if (write) {
        if (report.lost_found != 0) {
            kshell_printf("fsck: %u lost cluster(s) found, %u freed\n", (unsigned)report.lost_found,
                          (unsigned)report.lost_freed);
        }

        if (report.fixed != 0) {
            kshell_printf("fsck: %u repair(s) applied\n", (unsigned)report.fixed);
        }
    } else if (report.lost_clusters != 0) {
        kshell_printf("fsck: %u lost cluster(s)\n", (unsigned)report.lost_clusters);
    }

    if (report.errors != 0) {
        kshell_printf("fsck: %u problem(s), the first: %s%s\n", (unsigned)report.errors,
                      report.first, write ? " (not repaired)" : "");
        if (!write) {
            kshell_printf("fsck: -y repairs the lost clusters and cuts the broken chains\n");
        }
        return KSHELL_ERROR;
    }

    if (write && report.fixed != 0) {
        kshell_printf("fsck: 0 problem(s) after the repair\n");
    }

    kshell_printf("fsck: the volume is consistent\n");

    return KSHELL_OK;
}

/* The words after `fsck fat`: one point or device, and the flag of the
 * repair in any place. Anything else is the usage line, not a guess about
 * which of two volumes the person meant. */
static bool fsck_fat_words(int argc, char **argv, const char **what, bool *write)
{
    int i;

    *what = NULL;
    *write = false;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--repair") == 0) {
            *write = true;
        } else if (*what == NULL) {
            *what = argv[i];
        } else {
            *what = NULL;
            return false;
        }
    }

    return *what != NULL;
}
#endif

/*
 * The check of MicroFS, and its repair when `write` is set. The report of a
 * repair is about the volume after the pass: the counters are the volume as
 * it is now, `fixed` is what the pass changed and `errors` is what is still
 * wrong, which is why the words after -y are "after the repair" and not
 * "was repaired".
 */
static int fsck_mfs(bool write)
{
    struct mfs_check_report report;
    int rc;

    rc = write ? mfs_repair(&report) : mfs_check(&report);
    if (rc == MFS_EINVAL) {
        kshell_printf("fsck: no volume is mounted\n");
        return KSHELL_ERROR;
    }

    if (rc != MFS_OK) {
        kshell_printf("fsck: the volume cannot be read (%s)\n", mfs_strerror(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("fsck: %u file(s), %u directory(ies), %u of %u block(s) free\n",
                  (unsigned)report.files, (unsigned)report.directories,
                  (unsigned)report.free_blocks, (unsigned)report.data_blocks);

    if (write && report.fixed != 0) {
        kshell_printf("fsck: %u repair(s) applied\n", (unsigned)report.fixed);
    }

    if (report.errors != 0) {
        kshell_printf("fsck: %u problem(s), the first: %s%s\n", (unsigned)report.errors,
                      report.first, write ? " (not repaired)" : "");
        if (!write) {
            kshell_printf("fsck: -y repairs the orphan blocks, the entries and the chains\n");
        }
        return KSHELL_ERROR;
    }

    if (write && report.fixed != 0) {
        kshell_printf("fsck: 0 problem(s) after the repair\n");
    }

    kshell_printf("fsck: the volume is consistent\n");

    return KSHELL_OK;
}

static int cmd_fsck(int argc, char **argv)
{
    if (argc == 1) {
        return fsck_mfs(false);
    }

    if (argc == 2 && (strcmp(argv[1], "-y") == 0 || strcmp(argv[1], "--repair") == 0)) {
        return fsck_mfs(true);
    }

#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
    if (argc >= 3 && strcmp(argv[1], "fat") == 0) {
        const char *what;
        bool write;

        if (fsck_fat_words(argc, argv, &what, &write)) {
            return fsck_fat(what, write);
        }
    }
#endif

    kshell_printf("usage: %s\n", FSCK_USAGE);

    return KSHELL_USAGE;
}

static int cmd_path(int argc, char **argv)
{
    if (argc < 2) {
        kshell_printf("%s\n", g_shell_path);
        return KSHELL_OK;
    }

    if (argc > 2) {
        kshell_printf("usage: path [directories]\n");
        return KSHELL_USAGE;
    }

    /*
     * An empty value is not a directory: the placeholder `""` of the
     * splitter, which has no quotes, is refused like the empty argument an
     * API could hand the command. The default is a value a person can type
     * (and `path` without an argument prints it).
     */
    if (argv[1][0] == '\0' || strcmp(argv[1], "\"\"") == 0) {
        kshell_printf("path: an empty value is not a list of directories\n");
        return KSHELL_USAGE;
    }

    if (strlen(argv[1]) >= sizeof(g_shell_path)) {
        kshell_printf("path: the value is too long (%u %s at most)\n",
                      (unsigned)(sizeof(g_shell_path) - 1), byte_word(sizeof(g_shell_path) - 1));
        return KSHELL_ERROR;
    }

    snprintf(g_shell_path, sizeof(g_shell_path), "%s", argv[1]);

    return KSHELL_OK;
}

#if KERNEL_EXEC
/* Whether the name carries its own path, in which case PATH has no say. */
static bool shell_name_has_dir(const char *name)
{
    return strchr(name, '/') != NULL;
}

/*
 * The first entry of PATH that holds a file with an execute bit. A file
 * without one is passed over, as Linux does: a broken entry must not hide
 * the program that waits below it. The answer is the full path that will be
 * run, because the person has to see which file answered.
 */
static bool shell_find_program(const char *name, char *out, k_size_t size)
{
    const char *cursor = g_shell_path;

    for (;;) {
        const char *colon = strchr(cursor, ':');
        k_size_t len = (colon != NULL) ? (k_size_t)(colon - cursor) : strlen(cursor);
        char dir[SHELL_PATH_MAX];
        char candidate[SHELL_PATH_MAX];
        struct vfs_stat info;
        int n;

        /*
         * An empty entry is the current directory, as in Linux; an entry
         * too long to hold is not a directory that can be named.
         */
        if (len == 0) {
            dir[0] = '.';
            dir[1] = '\0';
            len = 1;
        } else if (len < sizeof(dir)) {
            memcpy(dir, cursor, len);
            dir[len] = '\0';
        } else {
            dir[0] = '\0';
        }

        if (dir[0] != '\0') {
            const char *slash = (dir[len - 1] == '/') ? "" : "/";

            n = snprintf(candidate, sizeof(candidate), "%s%s%s", dir, slash, name);
            if (n >= 0 && (k_size_t)n < sizeof(candidate) && shell_resolve(candidate, out, size) &&
                vfs_stat(out, &info) == 0 && info.type == 1 && (info.mode & 0111u) != 0) {
                return true;
            }
        }

        if (colon == NULL) {
            break;
        }
        cursor = colon + 1;
    }

    return false;
}

static int cmd_exec(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    /*
     * The vector is built on the stack of the shell: the line it points into
     * is a buffer of the shell as well, and a program of this system does not
     * read its arguments yet. The extra place is the null terminator.
     */
    char *args[KSHELL_MAX_ARGS + 1];
    int i;
    int rc;

    if (argc < 2) {
        kshell_printf("usage: exec <program> [arguments...]\n");
        return KSHELL_USAGE;
    }

    if (shell_name_has_dir(argv[1])) {
        if (!shell_resolve(argv[1], path, sizeof(path))) {
            return path_too_long("exec");
        }
    } else if (!shell_find_program(argv[1], path, sizeof(path))) {
        kshell_printf("exec: %s: not found in %s\n", argv[1], g_shell_path);
        return KSHELL_ERROR;
    }

    /*
     * argv[0] is the name that was typed, as in Linux, and the rest of the
     * line follows it. The line is already cut at KSHELL_MAX_ARGS words, so
     * the vector has its terminator inside the array.
     */
    for (i = 1; i < argc; i++) {
        args[i - 1] = argv[i];
    }
    args[argc - 1] = NULL;

    rc = microos_execve(path, args);
    if (rc == 0) {
        /* A program that returned: the machine has no process to exit into. */
        kshell_printf("the program has finished\n");
        return KSHELL_OK;
    }

    kshell_printf("exec: %s: %s\n", path, exec_error_text(microos_errno()));

    return KSHELL_ERROR;
}

/*
 * The PATH-first half of a command that is both a program and a command of
 * the shell: `ls` of the machine is /bin/ls when the machine has one. The
 * answer is 1 when the file was found and run; 0 keeps the command of the
 * shell, which is the answer of a simulator (no port to enter an image) and
 * of a board whose /bin has no file of that name.
 *
 * `execve` of this system runs the program as a task of its own and returns
 * when it has ended, so the shell waits without a `fork` of its own kernel
 * task (the port copies the address space of a program, not of the shell).
 * docs/debts.md has the POSIX shape.
 */
int kshell_try_program(int argc, char **argv, int *status)
{
    char path[SHELL_PATH_MAX];
    int rc;

    if (argc < 1 || argv == NULL || status == NULL) {
        return 0;
    }

    if (shell_name_has_dir(argv[0])) {
        if (!shell_resolve(argv[0], path, sizeof(path))) {
            return 0;
        }
    } else if (!shell_find_program(argv[0], path, sizeof(path))) {
        return 0;
    }

    rc = microos_execve(path, argv);
    if (rc == 0) {
        *status = KSHELL_OK;
        return 1;
    }

    /*
     * A machine whose port cannot enter a program answers ENOSYS (and a
     * machine of another arch ENOEXEC before a byte runs); the command of
     * the shell is the fallback, and the two are not a fault of the line.
     */
    if (microos_errno() == -KERR_NOSYS || microos_errno() == -KERR_NOTSUP) {
        return 0;
    }

    kshell_eprintf("%s: %s\n", argv[0], exec_error_text(microos_errno()));
    *status = KSHELL_ERROR;

    return 1;
}

/*
 * A stage of a pipeline whose name is not a command of the shell: the name
 * with a slash is a path, a name without one is looked up in PATH, and the
 * program runs with the descriptors the stage built for it. The answer is
 * the status of the stage: a program that ran and returned is a success,
 * because execve of this system waits for it and does not hand its code
 * back (docs/debts.md).
 */
int kshell_run_program(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    int rc;

    if (argc < 1 || argv == NULL) {
        return KSHELL_ERROR;
    }

    if (shell_name_has_dir(argv[0])) {
        if (!shell_resolve(argv[0], path, sizeof(path))) {
            kshell_eprintf("%s: path too long\n", argv[0]);
            return KSHELL_ERROR;
        }
    } else if (!shell_find_program(argv[0], path, sizeof(path))) {
        kshell_eprintf("%s: command not found. Type 'help' for the list.\n", argv[0]);
        return KSHELL_ERROR;
    }

    rc = microos_execve(path, argv);
    if (rc == 0) {
        return KSHELL_OK;
    }

    kshell_eprintf("%s: %s\n", argv[0], exec_error_text(microos_errno()));

    return KSHELL_ERROR;
}

#else  /* KERNEL_EXEC */
int kshell_try_program(int argc, char **argv, int *status)
{
    (void)argc;
    (void)argv;
    (void)status;

    return 0;
}

int kshell_run_program(int argc, char **argv)
{
    if (argc > 0 && argv != NULL) {
        kshell_eprintf("%s: command not found. Type 'help' for the list.\n", argv[0]);
    }

    return KSHELL_ERROR;
}
#endif /* KERNEL_EXEC */

/*
 * The commands of the files, with the whole help of the table. The usage
 * line is the one the option parser enforces and the detail carries the
 * flags, so that `-h`, `help <command>` and the list of `help` cannot tell
 * different stories about the same command.
 */
static const struct file_command {
    const char *name;
    struct kshell_help help;
    kshell_cmd_fn fn;
} g_file_commands[] = {
    {"pwd", {.summary = "the current directory", .usage = "pwd"}, cmd_pwd},
    {"cd", {.summary = "change the current directory", .usage = "cd <directory>"}, cmd_cd},
    {"ls",
     {.summary = "list the names of a directory",
      .usage = "ls [-la] [--color[=always|never|auto]] [path]",
      .detail = "  -l  the mode, the owner, the size and the date of every entry\n"
                "  -a  show . and .. first\n"
                "  --color[=when]  colour the names: blue for a directory and\n"
                "                  green for an executable; when is always,\n"
                "                  never or auto, and auto is off because the\n"
                "                  shell has no terminal to ask\n"
                "Without a path the current directory is listed. The names come one\n"
                "per line, in the order of the filesystem, and the listing has no\n"
                "fixed limit; `tree` draws the levels below a path.\n"},
     cmd_ls},
    {"tree",
     {.summary = "draw the tree of a directory",
      .usage = "tree [-ad] [path]",
      .detail = "  -a  accept the hidden names, of which this system has none yet\n"
                "  -d  draw the directories only; the files are counted all the same\n"
                "The walk has no fixed table: every entry of a directory is drawn or\n"
                "counted. It stops at the sixth level and says ... (deeper) there.\n"},
     cmd_tree},
    {"cat",
     {.summary = "print the content of a file, or of the input",
      .usage = "cat [-n] [file]",
      .detail = "  -n  number the lines, as cat of Linux does\n"
                "Without a file the standard input is copied, which is what makes\n"
                "`cat < file` and `ls | cat` work.\n"},
     cmd_cat},
    {"write",
     {.summary = "write a file",
      .usage = "write <file> <text...>",
      .detail = "The text is the rest of the line, joined by single spaces: the splitter\n"
                "of the shell has no quotes yet. The file is created or replaced.\n"},
     cmd_write},
    {"mkdir",
     {.summary = "create a directory",
      .usage = "mkdir <directory>",
      .detail = "The parent has to exist: there is no -p here yet.\n"},
     cmd_mkdir},
    {"rm",
     {.summary = "remove a file or a directory",
      .usage = "rm [-r] <path>",
      .detail = "  -r  remove a directory with everything in it\n"
                "Without -r a directory is refused, even an empty one: taking a tree\n"
                "down asks for the flag that says so. `rm -- -name` removes a file whose\n"
                "name begins with a dash.\n"},
     cmd_rm},
    {"df",
     {.summary = "the usage of the filesystem",
      .usage = "df [-h] [path]",
      .detail = "  -h  the sizes in KB and MB instead of blocks\n"
                "Without a path the volume of the current directory is shown.\n"
                "Here -h is the sizes, as in df of Linux: --help prints this.\n",
      .owns_short_h = true},
     cmd_df},
    {"mount",
     {.summary = "list, mount or unmount the filesystems",
      .usage = "mount [-a] [<device> <point> <type>]",
      .detail = "Without a word the mounted filesystems are listed, one point per line.\n"
                "  -a  mount every volume of " VFS_FSTAB_PATH "\n"
                "The words are `device point type`, as in the table and in Linux.\n"},
     cmd_mount},
    {"umount",
     {.summary = "take a filesystem out of the tree",
      .usage = "umount <point>",
      .detail = "A point with a mount below it stays until the smaller one leaves.\n"
                "`/` is the root and stays; the volume is formatted by mkfs -y where\n"
                "the machine has it, or by the image of a machine that does not.\n"
                "`/proc` and `/dev` are mounted by the kernel and stay as well.\n"},
     cmd_umount},
    {"fsck",
     {.summary = "check the volume, or repair it with -y",
      .usage = FSCK_USAGE,
      .detail = FSCK_DETAIL},
     cmd_fsck},
    {"chmod",
     {.summary = "change the mode of a file",
      .usage = "chmod <mode> <path>",
      .detail = "The mode is the nine bits in octal: 0644, 755, 0700. A zero is refused,\n"
                "because a record without a mode means one written before the modes.\n"},
     cmd_chmod},
    {"chown",
     {.summary = "change the owner of a file",
      .usage = "chown <uid> <path>",
      .detail = "The uid is a decimal number, 0..32767.\n"},
     cmd_chown},
    {"stat",
     {.summary = "the type, the mode, the owner and the size",
      .usage = "stat <path>",
      .detail = "Prints the line of `ls -l` first and then the same numbers as fields, so\n"
                "that a chmod or a chown is checked without a calculator.\n"},
     cmd_stat},
    {"path",
     {.summary = "show or set PATH",
      .usage = "path [directories]",
      .detail = "Without a word the value is printed. With one it is set, the directories\n"
                "separated by ':'. An empty value is not a list of directories and is\n"
                "refused; the default is /bin:/usr/bin.\n"},
     cmd_path},
#if KERNEL_EXEC
    {"exec",
     {.summary = "run a program",
      .usage = "exec <program> [arguments...]",
      .detail = "A name with a slash is a path; a name without one is looked up in PATH.\n"
                "The file must be a MOX1 image with an execute bit, and not every machine\n"
                "can run one: the answer says which of the two it was.\n"},
     cmd_exec},
#endif
};

int kshell_register_files(void)
{
    k_size_t i;
    int rc = 0;

    for (i = 0; i < K_ARRAY_SIZE(g_file_commands); i++) {
        rc |= kshell_register_help(g_file_commands[i].name, &g_file_commands[i].help,
                                   g_file_commands[i].fn);
    }

    return rc;
}
