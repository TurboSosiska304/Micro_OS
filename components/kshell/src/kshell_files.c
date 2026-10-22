/*
 * Micro OS — the commands of the shell that work with the files.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kernel/task.h"
#include "kernel/vfs.h"
#include "libc/unistd.h"

#define SHELL_MAX_ENTRIES 64
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

    for (i = 0; i < count; i++) {
        if (entries[i].type == 2) {
            kshell_printf("d %-24s\n", entries[i].name);
        } else {
            kshell_printf("- %-24s %6u B\n", entries[i].name, (unsigned)entries[i].size);
        }
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
        kshell_printf("write: the text does not fit, it is cut at %u byte(s)\n",
                      (unsigned)len);
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

    return rc;
}
