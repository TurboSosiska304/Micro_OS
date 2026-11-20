/*
 * Micro OS — the commands of the shell that work with the files: the tree, the
 * content, the mode and the owner, and the file that becomes a program.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "libc/stdlib.h"
#include "libc/unistd.h"

#define SHELL_MAX_ENTRIES 64
#define SHELL_LS_CHILDREN 16
#define SHELL_PATH_MAX 128

/*
 * The value of PATH. It belongs to the shell and not to the filesystem or
 * the kernel: which directories hold the commands is a matter of the person
 * at the console, and a program of this system does not inherit it yet.
 */
#define SHELL_PATH_DEFAULT "/bin:/usr/bin"
#define SHELL_PATH_SIZE 96

static char g_shell_path[SHELL_PATH_SIZE] = SHELL_PATH_DEFAULT;

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
 * Put a path together with the current directory. Returns false when the
 * path does not fit: a path that is cut is a different path, and a
 * command that works with the cut path works with the wrong file.
 */
static bool shell_resolve(const char *path, char *out, k_size_t size)
{
    const char *cwd = shell_cwd();
    int n;

    if (size == 0) {
        return false;
    }

    if (path == NULL) {
        n = snprintf(out, size, "%s", cwd);
    } else if (path[0] == '/') {
        n = snprintf(out, size, "%s", path);
    } else if (strcmp(cwd, "/") == 0) {
        n = snprintf(out, size, "/%s", path);
    } else {
        n = snprintf(out, size, "%s/%s", cwd, path);
    }

    return n >= 0 && (k_size_t)n < size;
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

/* One line of the tree: the branch, the name of the entry, a slash for a dir. */
static void ls_print(const char *name, k_size_t type, const char *prefix, bool last)
{
    kshell_printf("%s%s %s%s\n", prefix, last ? "`--" : "|--", name, type == 2 ? "/" : "");
}

/*
 * The entries below one directory of the listing, indented under it: the
 * picture of the root then shows /etc and /etc/init.d at once, while the
 * table of the level above stays in its own static place.
 */
static void ls_children(const char *path, const char *prefix)
{
    static struct vfs_dirent entries[SHELL_LS_CHILDREN];
    k_size_t count = 0;
    k_size_t i;

    if (vfs_list(path, entries, SHELL_LS_CHILDREN, &count) != 0) {
        return;
    }

    for (i = 0; i < count; i++) {
        ls_print(entries[i].name, entries[i].type, prefix, i + 1 == count);
    }
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

static int cmd_ls(int argc, char **argv)
{
    /*
     * The table of the entries is 3 KB: too much for the stack of a task,
     * which is 8 KB. The shell is one task and does not run two listings
     * at the same time.
     */
    static struct vfs_dirent entries[SHELL_MAX_ENTRIES];
    char path[SHELL_PATH_MAX];
    k_size_t count = 0;
    k_size_t i;
    int rc;

    if (!shell_resolve(argc > 1 ? argv[1] : NULL, path, sizeof(path))) {
        return path_too_long("ls");
    }

    rc = vfs_list(path, entries, SHELL_MAX_ENTRIES, &count);
    if (rc != 0) {
        kshell_printf("ls: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("%s\n", path);

    for (i = 0; i < count; i++) {
        char child_path[SHELL_PATH_MAX];
        char child_prefix[16];
        bool last = (i + 1 == count);

        ls_print(entries[i].name, entries[i].type, "", last);

        if (entries[i].type != 2) {
            continue;
        }

        /* The path of the child, and the prefix its own entries start with. */
        if (!ls_child_path(path, entries[i].name, child_path, sizeof(child_path))) {
            continue;
        }
        snprintf(child_prefix, sizeof(child_prefix), "%s", last ? "    " : "|   ");

        ls_children(child_path, child_prefix);
    }

    if (count == 0) {
        kshell_printf("(empty)\n");
    }

    return KSHELL_OK;
}

static int cmd_cat(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    char buffer[128];
    int fd;

    if (argc < 2) {
        kshell_printf("usage: cat <file>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("cat");
    }

    /*
     * The file is opened through the library of the system, that is, through
     * the calls of the kernel. The shell and the programs of the user see
     * the same files.
     */
    fd = microos_open(path, MICROOS_O_RDONLY);
    if (fd < 0) {
        kshell_printf("cat: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    for (;;) {
        long got = microos_read(fd, buffer, sizeof(buffer));

        if (got == 0) {
            break; /* the end of the file */
        }

        if (got < 0) {
            /* A failed read is not the end of the file: it has to be said. */
            kshell_printf("cat: %s: %s\n", path, microos_strerror(microos_errno()));
            microos_close(fd);
            return KSHELL_ERROR;
        }

        kshell_printf("%.*s", (int)got, buffer);
    }

    if (microos_close(fd) != 0) {
        kshell_printf("cat: %s: %s\n", path, microos_strerror(microos_errno()));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
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
        kshell_printf("write: the text does not fit, it is cut at %u byte(s)\n", (unsigned)len);
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

    kshell_printf("%u byte(s) written to %s\n", (unsigned)(len + 1), path);

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
        kshell_printf("mkdir: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int cmd_rm(int argc, char **argv)
{
    char path[SHELL_PATH_MAX];
    int rc;

    if (argc < 2) {
        kshell_printf("usage: rm <file>\n");
        return KSHELL_USAGE;
    }

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("rm");
    }

    rc = vfs_unlink(path);
    if (rc != 0) {
        kshell_printf("rm: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int cmd_df(int argc, char **argv)
{
    struct vfs_statfs stat;
    char path[SHELL_PATH_MAX];
    int rc;

    (void)argc;

    if (!shell_resolve(argv[1] != NULL ? argv[1] : NULL, path, sizeof(path))) {
        return path_too_long("df");
    }

    rc = vfs_statfs(path, &stat);
    if (rc != 0) {
        kshell_printf("df: %s: %s\n", path, vfs_strerror(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("blocks: %u of %u free (%u KB each)\n", (unsigned)stat.free_blocks,
                  (unsigned)stat.total_blocks, (unsigned)(stat.block_size / 1024));
    kshell_printf("inodes: %u of %u free\n", (unsigned)stat.free_inodes,
                  (unsigned)stat.total_inodes);

    return KSHELL_OK;
}

static int cmd_mount(int argc, char **argv)
{
    k_size_t count = vfs_mount_count();
    k_size_t i;

    (void)argc;
    (void)argv;

    if (count == 0) {
        kshell_printf("no filesystem is mounted\n");
        return KSHELL_OK;
    }

    for (i = 0; i < count; i++) {
        kshell_printf("%s\n", vfs_mount_name(i));
    }

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
        return "no such file";
    case -KERR_INVAL:
        return invalid;
    default:
        return microos_strerror(code);
    }
}

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
 * The consistency of the volume, read-only. The filesystem answers with
 * the report and not with a repair: a person decides what to do about a
 * broken record, and a checker that writes is a formatter that nobody
 * asked for.
 */
static int cmd_fsck(int argc, char **argv)
{
    struct mfs_check_report report;
    int rc;

    K_UNUSED(argv);

    if (argc != 1) {
        kshell_printf("usage: fsck\n");
        return KSHELL_USAGE;
    }

    rc = mfs_check(&report);
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
                  (unsigned)report.free_blocks, (unsigned)report.used_blocks);

    if (report.errors != 0) {
        kshell_printf("fsck: %u problem(s), the first: %s\n", (unsigned)report.errors,
                      report.first);
        return KSHELL_ERROR;
    }

    kshell_printf("fsck: the volume is consistent\n");

    return KSHELL_OK;
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
     * The splitter of the shell has no quotes yet, so the empty argument
     * that resets the value reaches this command as two quote marks: that
     * is the only way a person can type "nothing" at the console.
     */
    if (argv[1][0] == '\0' || strcmp(argv[1], "\"\"") == 0) {
        snprintf(g_shell_path, sizeof(g_shell_path), "%s", SHELL_PATH_DEFAULT);
        return KSHELL_OK;
    }

    if (strlen(argv[1]) >= sizeof(g_shell_path)) {
        kshell_printf("path: the value is too long (%u byte(s) at most)\n",
                      (unsigned)(sizeof(g_shell_path) - 1));
        return KSHELL_ERROR;
    }

    snprintf(g_shell_path, sizeof(g_shell_path), "%s", argv[1]);

    return KSHELL_OK;
}

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

int kshell_register_files(void)
{
    int rc = 0;

    rc |= kshell_register("pwd", "the current directory", cmd_pwd);
    rc |= kshell_register("cd", "change the current directory", cmd_cd);
    rc |= kshell_register("ls", "list the files of a directory", cmd_ls);
    rc |= kshell_register("cat", "print the content of a file", cmd_cat);
    rc |= kshell_register("write", "write a file: write <file> <text>", cmd_write);
    rc |= kshell_register("mkdir", "create a directory", cmd_mkdir);
    rc |= kshell_register("rm", "remove a file", cmd_rm);
    rc |= kshell_register("df", "the usage of the filesystem", cmd_df);
    rc |= kshell_register("mount", "the mounted filesystems", cmd_mount);
    rc |= kshell_register("fsck", "check the volume, read-only", cmd_fsck);
    rc |= kshell_register("chmod", "change the mode of a file", cmd_chmod);
    rc |= kshell_register("chown", "change the owner of a file", cmd_chown);
    rc |= kshell_register("stat", "the type, the mode and the owner", cmd_stat);
    rc |= kshell_register("path", "show or set PATH: path [directories]", cmd_path);
    rc |= kshell_register("exec", "run a program: exec <program> [args...]", cmd_exec);

    return rc;
}
