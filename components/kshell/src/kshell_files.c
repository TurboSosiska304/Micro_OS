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
#include "libc/stdlib.h"
#include "libc/unistd.h"

#define SHELL_MAX_ENTRIES 64
#define SHELL_LS_CHILDREN 16
#define SHELL_PATH_MAX 128

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

/* --- the mode, the owner and the programs --- */

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

    if (!shell_resolve(argv[1], path, sizeof(path))) {
        return path_too_long("exec");
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
    rc |= kshell_register("chmod", "change the mode of a file", cmd_chmod);
    rc |= kshell_register("chown", "change the owner of a file", cmd_chown);
    rc |= kshell_register("exec", "run a program: exec <path> [args...]", cmd_exec);

    return rc;
}
