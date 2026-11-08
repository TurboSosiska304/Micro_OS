/*
 * Micro OS — the init scripts.
 *
 * The board has no execve: the "programs" of the system are the commands of
 * the shell. A script of /etc/init.d is a list of those commands, one per
 * line, and the comments start with '#' as in the shell script of Linux.
 */
#include "init/initd.h"

#include <string.h>

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"

/* The size of one read of a script; the lines are assembled from it. */
#define INITD_READ_CHUNK 64

/* The place of one line: one command of the shell, as in the shell itself. */
#define INITD_LINE_MAX 128

/*
 * Run one line of a script. The spaces around it are not a part of the
 * command, and a comment is not a command at all.
 */
static int run_line(char *line)
{
    char *start = line;
    k_size_t len;

    while (*start == ' ' || *start == '\t') {
        start++;
    }

    len = strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
        start[--len] = '\0';
    }

    if (len == 0 || start[0] == '#') {
        return KSHELL_OK;
    }

    return kshell_execute(start);
}

/*
 * Create the directory and every parent of it: /etc/init.d needs /etc
 * first, and a fresh volume has neither of them.
 */
static int ensure_dir(const char *dir)
{
    char path[VFS_PATH_MAX];
    k_size_t len;
    k_size_t i;
    int rc;

    if (dir == NULL) {
        return -1;
    }

    len = strlen(dir);
    if (len == 0 || len >= sizeof(path)) {
        return -1;
    }

    memcpy(path, dir, len + 1);

    for (i = 1; i <= len; i++) {
        char saved;

        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }

        saved = path[i];
        path[i] = '\0';

        rc = vfs_mkdir(path);
        if (rc != 0 && rc != -3) {
            path[i] = saved;
            return rc;
        }

        path[i] = saved;
    }

    return 0;
}

int initd_run_script(const char *path)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char line[INITD_LINE_MAX];
    char chunk[INITD_READ_CHUNK];
    k_size_t used = 0;
    k_size_t total = 0;
    int skip = 0;
    int result = 0;
    int rc;

    if (path == NULL) {
        return -1;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        kwarn("init: cannot open the script %s (rc=%d)", path, rc);
        return rc;
    }

    for (;;) {
        int n = vfs_read(&file, chunk, sizeof(chunk));
        int i;

        if (n < 0) {
            result = n;
            break;
        }
        if (n == 0) {
            break;
        }

        total += (k_size_t)n;
        if (total > INITD_SCRIPT_MAX) {
            kwarn("init: the script %s is longer than %d bytes, the rest is ignored", path,
                  INITD_SCRIPT_MAX);
            break;
        }

        for (i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                if (!skip) {
                    line[used] = '\0';
                    if (run_line(line) != KSHELL_OK) {
                        kwarn("init: a command of %s failed", path);
                    }
                }
                used = 0;
                skip = 0;
                continue;
            }

            if (skip) {
                continue;
            }

            if (used + 1 < sizeof(line)) {
                line[used++] = chunk[i];
            } else {
                kwarn("init: a line of %s is longer than %d characters", path, INITD_LINE_MAX - 1);
                skip = 1;
            }
        }
    }

    if (!skip && used > 0) {
        line[used] = '\0';
        if (run_line(line) != KSHELL_OK) {
            kwarn("init: a command of %s failed", path);
        }
    }

    vfs_close(&file);

    return result;
}

/* The task of one script: run the commands, free the path, end. */
static void script_task(void *arg)
{
    char *path = arg;

    (void)initd_run_script(path);
    kmm_free(path);
    task_exit(0);
}

static int start_script(const char *dir, const char *name)
{
    char *path;
    k_size_t dlen = strlen(dir);
    k_size_t nlen = strlen(name);
    struct task *task;

    if (dlen + 1 + nlen + 1 > VFS_PATH_MAX) {
        kwarn("init: the path of the script %s/%s is too long", dir, name);
        return -1;
    }

    path = kmm_alloc(dlen + 1 + nlen + 1, KMM_PRIORITY_SYSTEM);
    if (path == NULL) {
        kwarn("init: no memory for the script %s/%s", dir, name);
        return -1;
    }

    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, name, nlen);
    path[dlen + 1 + nlen] = '\0';

    kinfo("init: starting %s", path);

    task = task_create(name, script_task, path, TASK_PRIO_DEFAULT);
    if (task == NULL) {
        kwarn("init: cannot start the script %s", path);
        kmm_free(path);
        return -1;
    }

    return 0;
}

int initd_run(const char *dir)
{
    struct vfs_dirent entries[INITD_MAX_SCRIPTS];
    k_size_t count = 0;
    k_size_t i;
    int started = 0;
    int rc;

    if (dir == NULL) {
        return -1;
    }

    rc = ensure_dir(dir);
    if (rc != 0) {
        kwarn("init: cannot create the directory %s (rc=%d)", dir, rc);
        return rc;
    }

    rc = vfs_list(dir, entries, K_ARRAY_SIZE(entries), &count);
    if (rc != 0) {
        kwarn("init: cannot read the directory %s (rc=%d)", dir, rc);
        return rc;
    }

    for (i = 0; i < count; i++) {
        if (entries[i].type != 1 || entries[i].name[0] == '.') {
            continue;
        }

        if (start_script(dir, entries[i].name) == 0) {
            started++;
        }
    }

    return started;
}
