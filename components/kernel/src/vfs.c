#include "kernel/vfs.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

struct mount {
    char point[VFS_PATH_MAX];
    const struct vfs_operations *ops;
    void *ctx;
};

#define VFS_MAX_FILES 16
static struct mount g_mounts[VFS_MAX_MOUNTS];
static k_size_t g_count;
static struct vfs_fs_type g_types[VFS_MAX_TYPES];
static k_size_t g_type_count;
static int g_last_error;

static void copy_str(char *dst, k_size_t size, const char *src)
{
    k_size_t len;

    if (size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int vfs_mount(const char *point, const struct vfs_operations *ops, void *ctx)
{
    struct mount *slot = NULL;
    k_size_t i;

    if (point == NULL || ops == NULL) {
        return KERR_INVAL;
    }

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mounts[i].point, point) == 0) {
            /* The filesystem is already mounted, replace it. A filesystem
             * that keeps a volume in a static slot arranges its own
             * release before it comes here (fat_vfs_mount does). */
            g_mounts[i].ops = ops;
            g_mounts[i].ctx = ctx;
            return 0;
        }
    }

    if (g_count >= VFS_MAX_MOUNTS) {
        kerror("vfs: there is no room for another filesystem");
        return KERR_NOSPC;
    }

    slot = &g_mounts[g_count];
    copy_str(slot->point, sizeof(slot->point), point);
    slot->ops = ops;
    slot->ctx = ctx;
    g_count++;

    kinfo("vfs: '%s' is mounted", slot->point);

    return 0;
}

int vfs_umount(const char *point)
{
    k_size_t i;

    if (point == NULL) {
        return KERR_INVAL;
    }

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mounts[i].point, point) == 0) {
            int rc = 0;

            if (g_mounts[i].ops->umount != NULL) {
                rc = g_mounts[i].ops->umount(g_mounts[i].ctx);
                if (rc != 0) {
                    return rc;
                }
            }

            for (; i + 1 < g_count; i++) {
                g_mounts[i] = g_mounts[i + 1];
            }
            g_count--;
            return 0;
        }
    }

    return KERR_NOENT;
}

int vfs_register_type(const char *name, vfs_mount_fn mount)
{
    k_size_t len;
    k_size_t i;

    if (name == NULL || mount == NULL) {
        return KERR_INVAL;
    }

    len = strlen(name);
    if (len == 0 || len > VFS_TYPE_NAME_MAX) {
        return KERR_INVAL;
    }

    for (i = 0; i < g_type_count; i++) {
        if (strcmp(g_types[i].name, name) == 0) {
            g_types[i].mount = mount;
            return 0;
        }
    }

    if (g_type_count >= VFS_MAX_TYPES) {
        kerror("vfs: there is no room for another type of filesystem");
        return KERR_NOSPC;
    }

    memcpy(g_types[g_type_count].name, name, len + 1);
    g_types[g_type_count].mount = mount;
    g_type_count++;

    kinfo("vfs: the type '%s' is registered", name);

    return 0;
}

int vfs_unregister_type(const char *name)
{
    k_size_t i;

    if (name == NULL) {
        return KERR_INVAL;
    }

    for (i = 0; i < g_type_count; i++) {
        if (strcmp(g_types[i].name, name) == 0) {
            for (; i + 1 < g_type_count; i++) {
                g_types[i] = g_types[i + 1];
            }
            g_type_count--;
            return 0;
        }
    }

    return KERR_NOENT;
}

k_size_t vfs_type_count(void)
{
    return g_type_count;
}

const char *vfs_type_name(k_size_t index)
{
    if (index >= g_type_count) {
        return NULL;
    }

    return g_types[index].name;
}

int vfs_mount_type(const char *point, const char *type, const char *device)
{
    k_size_t i;

    if (point == NULL || type == NULL) {
        return KERR_INVAL;
    }

    for (i = 0; i < g_type_count; i++) {
        if (strcmp(g_types[i].name, type) == 0) {
            return g_types[i].mount(point, device);
        }
    }

    kerror("vfs: there is no type '%s'", type);

    return KERR_INVAL;
}

k_size_t vfs_mount_count(void)
{
    return g_count;
}

const char *vfs_mount_name(k_size_t index)
{
    if (index >= g_count) {
        return NULL;
    }

    return g_mounts[index].point;
}

/*
 * Find the filesystem of a path: the mount with the longest matching
 * prefix. The rest of the path belongs to the filesystem.
 */
static const struct mount *resolve(const char *path, const char **relative)
{
    const struct mount *best = NULL;
    k_size_t best_len = 0;
    k_size_t i;

    if (path == NULL) {
        return NULL;
    }

    for (i = 0; i < g_count; i++) {
        k_size_t len = strlen(g_mounts[i].point);

        if (strncmp(path, g_mounts[i].point, len) != 0) {
            continue;
        }

        /*
         * The point has to be a whole directory of the path. The root is
         * the prefix of every path, so it always matches.
         */
        if (len > 1 && path[len] != '\0' && path[len] != '/') {
            continue;
        }

        if (best == NULL || len > best_len) {
            best = &g_mounts[i];
            best_len = len;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    if (relative != NULL) {
        const char *rest = path + best_len;

        if (*rest == '\0') {
            *relative = "/";
        } else if (*rest == '/') {
            *relative = rest;
        } else {
            *relative = rest;
        }
    }

    return best;
}

/* The bits of one triad of a mode: read, write, execute. */
#define VFS_MODE_EXEC 01u

/*
 * The bits of a mode that decide for the caller, which is the one user of
 * the system (VFS_UID). There is no bypass for that uid: an object of another
 * owner is answered by the triad of the others, because the caller is not its
 * owner and the system has no group of its own (ADR 0005).
 */
static k_u32_t caller_bits(const struct vfs_stat *info)
{
    return (info->owner == VFS_UID) ? ((info->mode >> 6) & 7u) : (info->mode & 7u);
}

/*
 * The information about an object, without the walk of the path around it.
 * The public vfs_stat() checks the directories of the path first and then
 * asks here, so this is the call that the walk itself may use: a walk that
 * went through vfs_stat() would walk the same path again, for ever.
 */
static int stat_raw(const char *path, struct vfs_stat *out)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    if (out == NULL) {
        g_last_error = KERR_INVAL;
        return KERR_INVAL;
    }

    if (m->ops->stat == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    /*
     * The answer is zeroed before the filesystem fills it: a filesystem that
     * keeps no modes leaves the zero that says so, and the bytes of the
     * answer are the same for every call - stat is copied to the memory of
     * the caller whole, padding included.
     */
    memset(out, 0, sizeof(*out));

    rc = m->ops->stat(m->ctx, path, out);
    g_last_error = rc;

    return rc;
}

/*
 * The information about an object with the moments of a change. A
 * filesystem that keeps no time is read through its stat operation: the
 * two moments stay zero, which is the "unknown" of the contract and not a
 * moment of 1970.
 */
static int statx_raw(const char *path, struct vfs_statx *out)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    if (out == NULL) {
        g_last_error = KERR_INVAL;
        return KERR_INVAL;
    }

    memset(out, 0, sizeof(*out));

    if (m->ops->statx != NULL) {
        rc = m->ops->statx(m->ctx, path, out);
        g_last_error = rc;
        return rc;
    }

    if (m->ops->stat != NULL) {
        struct vfs_stat info;

        rc = m->ops->stat(m->ctx, path, &info);
        if (rc != 0) {
            g_last_error = rc;
            return rc;
        }

        out->inode = info.inode;
        out->type = info.type;
        out->size = info.size;
        out->mode = info.mode;
        out->owner = info.owner;
        out->mtime = 0;
        out->ctime = 0;
        g_last_error = 0;
        return 0;
    }

    g_last_error = KERR_NOTSUP;
    return KERR_NOTSUP;
}

/*
 * A mount point is a directory of the tree even when the filesystem below
 * it cannot describe it: devfs answers ENOENT for its own root. Such a
 * filesystem keeps no modes, so the search through its root is not denied.
 */
static bool is_mount_point(const char *path)
{
    k_size_t i;

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mounts[i].point, path) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * One directory of a path has to be searchable by the caller: the walk of a
 * path asks the `x` bit of every directory on the way before the object at
 * the end is looked at. A directory of the mode 0600 therefore cannot let
 * anybody reach its children, and one of 0700 can.
 *
 * A zeroed mode is not "nobody may search it": it means that the filesystem
 * keeps no modes at all (procfs, devfs), and such a directory is walked the
 * way it always was.
 */
static int searchable_check(const char *prefix)
{
    struct vfs_stat info;
    int rc;

    rc = stat_raw(prefix, &info);
    if (rc == KERR_NOENT && is_mount_point(prefix)) {
        return 0;
    }

    if (rc != 0) {
        return rc;
    }

    if (info.type != 2) {
        return KERR_NOTDIR;
    }

    if (info.mode == 0u) {
        return 0;
    }

    return ((caller_bits(&info) & VFS_MODE_EXEC) != 0u) ? 0 : KERR_PERM;
}

/*
 * Walk the directories of a path. The object at the end is not checked: the
 * search bit belongs to the directories that are passed through, not to the
 * one that is named, and the access to the object itself is the business of
 * the call that wants it (open checks the read and the write bits).
 *
 * The walk is the same for a call that creates, removes or changes an object:
 * the parent is the last directory that is passed through, and reaching it is
 * what those calls need. The object itself keeps the check it always had,
 * which belongs to the filesystem and not to the tree above it.
 */
static int path_search_check(const char *path)
{
    char prefix[VFS_PATH_MAX];
    k_size_t len;
    k_size_t i;
    int rc;

    if (path == NULL) {
        return KERR_INVAL;
    }

    len = strlen(path);
    if (len >= sizeof(prefix)) {
        return KERR_NAMETOOLONG;
    }

    /* The root itself is passed through by every other path. */
    if (len <= 1) {
        return 0;
    }

    rc = searchable_check("/");
    if (rc != 0) {
        return rc;
    }

    for (i = 1; i < len; i++) {
        if (path[i] != '/') {
            continue;
        }

        memcpy(prefix, path, i);
        prefix[i] = '\0';

        rc = searchable_check(prefix);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

int vfs_open(const char *path, int flags, struct vfs_file *out, void *priv)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL || out == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    if (m->ops->open == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    /* The descriptor of the caller is not initialised: the offset and the
     * size have to start from zero. The private data belongs to the caller:
     * the kernel keeps them with the open file of the task. */
    memset(out, 0, sizeof(*out));
    out->priv = priv;
    out->ops = m->ops;
    out->fs = m->ctx;
    out->flags = flags;

    rc = m->ops->open(m->ctx, path, flags, out);
    g_last_error = rc;

    if (rc != 0) {
        out->ops = NULL;
        out->fs = NULL;
    }

    return rc;
}

int vfs_read(struct vfs_file *file, void *buf, k_size_t size)
{
    int rc;

    if (file == NULL || file->ops == NULL) {
        return KERR_INVAL;
    }

    /* A directory is read through readdir, not as a stream of bytes. */
    if ((file->flags & VFS_O_DIRECTORY) != 0) {
        g_last_error = KERR_ISDIR;
        return KERR_ISDIR;
    }

    if (file->ops->read == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = file->ops->read(file->fs, file, buf, size);
    g_last_error = rc;

    return rc;
}

int vfs_write(struct vfs_file *file, const void *buf, k_size_t size)
{
    int rc;

    if (file == NULL || file->ops == NULL) {
        return KERR_INVAL;
    }

    /* A directory is read through readdir and never written. */
    if ((file->flags & VFS_O_DIRECTORY) != 0) {
        g_last_error = KERR_ISDIR;
        return KERR_ISDIR;
    }

    if (file->ops->write == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = file->ops->write(file->fs, file, buf, size);
    g_last_error = rc;

    return rc;
}

int vfs_close(struct vfs_file *file)
{
    int rc;

    if (file == NULL || file->ops == NULL) {
        return KERR_INVAL;
    }

    if (file->ops->close == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = file->ops->close(file->fs, file);
    g_last_error = rc;

    return rc;
}

/*
 * The name of the mount point `point` when it is a direct child of `dir`,
 * NULL otherwise. The root is the parent of every first level point.
 */
static const char *mount_child_name(const char *dir, const char *point)
{
    k_size_t len = strlen(dir);

    if (len == 1 && dir[0] == '/') {
        if (point[0] != '/' || point[1] == '\0') {
            return NULL;
        }
        if (strchr(point + 1, '/') != NULL) {
            return NULL;
        }
        return point + 1;
    }

    if (strncmp(dir, point, len) != 0 || point[len] != '/') {
        return NULL;
    }
    if (point[len + 1] == '\0' || strchr(point + len + 1, '/') != NULL) {
        return NULL;
    }

    return point + len + 1;
}

static bool already_listed(const struct vfs_dirent *out, k_size_t count, const char *name)
{
    k_size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(out[i].name, name) == 0) {
            return true;
        }
    }

    return false;
}

int vfs_list(const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count)
{
    const struct mount *m = resolve(path, NULL);
    k_size_t used = 0;
    k_size_t i;
    int rc;

    if (out == NULL || count == NULL) {
        g_last_error = -1;
        return -1;
    }

    *count = 0;

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    if (m != NULL) {
        if (m->ops->list == NULL) {
            g_last_error = KERR_NOTSUP;
            return KERR_NOTSUP;
        }

        rc = m->ops->list(m->ctx, path, out, max, &used);
        if (rc != 0) {
            g_last_error = rc;
            return rc;
        }
    }

    /*
     * A filesystem mounted below this directory is a directory of it, even
     * when the filesystem of the directory itself knows nothing about the
     * mount point. This is how /proc and /dev appear in the root.
     */
    for (i = 0; i < g_count && used < max; i++) {
        const char *name = mount_child_name(path, g_mounts[i].point);
        k_size_t nlen;

        if (name == NULL || already_listed(out, used, name)) {
            continue;
        }

        nlen = strlen(name);
        if (nlen > VFS_NAME_MAX) {
            continue;
        }

        memcpy(out[used].name, name, nlen + 1);
        out[used].inode = 0;
        out[used].type = 2;
        out[used].size = 0;
        used++;
    }

    if (m == NULL && used == 0) {
        g_last_error = -2;
        return -2;
    }

    *count = used;
    g_last_error = 0;

    return 0;
}

int vfs_readdir(struct vfs_file *file, struct vfs_dirent *out, k_size_t max, k_size_t *count)
{
    int rc;

    if (out == NULL || count == NULL) {
        g_last_error = KERR_INVAL;
        return KERR_INVAL;
    }

    *count = 0;

    /*
     * The position of a directory lives in the descriptor, so the call has
     * to be the one of a descriptor that was opened with VFS_O_DIRECTORY.
     * Anything else - a file, a filesystem without directories - is not a
     * directory to read.
     */
    if (file == NULL || file->ops == NULL || (file->flags & VFS_O_DIRECTORY) == 0) {
        g_last_error = KERR_NOTDIR;
        return KERR_NOTDIR;
    }

    if (file->ops->readdir == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = file->ops->readdir(file->fs, file, out, max, count);
    g_last_error = rc;

    return rc;
}

int vfs_stat(const char *path, struct vfs_stat *out)
{
    int rc;

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    return stat_raw(path, out);
}

int vfs_statx(const char *path, struct vfs_statx *out)
{
    int rc;

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    return statx_raw(path, out);
}

int vfs_mkdir(const char *path)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    if (m->ops->mkdir == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = m->ops->mkdir(m->ctx, path);
    g_last_error = rc;

    return rc;
}

int vfs_mkdir_p(const char *path)
{
    char buffer[VFS_PATH_MAX];
    k_size_t len;
    k_size_t i;
    int rc;

    if (path == NULL) {
        g_last_error = -1;
        return -1;
    }

    len = strlen(path);
    if (len == 0 || len >= sizeof(buffer)) {
        g_last_error = -1;
        return -1;
    }

    memcpy(buffer, path, len + 1);

    for (i = 1; i <= len; i++) {
        char saved;

        if (buffer[i] != '/' && buffer[i] != '\0') {
            continue;
        }

        saved = buffer[i];
        buffer[i] = '\0';
        rc = vfs_mkdir(buffer);
        buffer[i] = saved;

        if (rc != 0 && rc != -3) {
            g_last_error = rc;
            return rc;
        }
    }

    return 0;
}

int vfs_statfs(const char *path, struct vfs_statfs *out)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL || out == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    if (m->ops->statfs == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = m->ops->statfs(m->ctx, out);
    g_last_error = rc;

    return rc;
}

int vfs_unlink(const char *path)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    if (m->ops->unlink == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = m->ops->unlink(m->ctx, path);
    g_last_error = rc;

    return rc;
}

int vfs_chmod(const char *path, k_u32_t mode)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    /*
     * A filesystem without modes answers KERR_NOTSUP, the same as every
     * operation it does not have: the call is not forbidden, the object has
     * no such attribute. A number stored nowhere would be a lie to the
     * caller.
     */
    if (m->ops->chmod == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = m->ops->chmod(m->ctx, path, mode);
    g_last_error = rc;

    return rc;
}

int vfs_chown(const char *path, k_u32_t owner)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = KERR_NOENT;
        return KERR_NOENT;
    }

    rc = path_search_check(path);
    if (rc != 0) {
        g_last_error = rc;
        return rc;
    }

    if (m->ops->chown == NULL) {
        g_last_error = KERR_NOTSUP;
        return KERR_NOTSUP;
    }

    rc = m->ops->chown(m->ctx, path, owner);
    g_last_error = rc;

    return rc;
}

const char *vfs_strerror(int error)
{
    k_size_t i;

    /* The refusal of the VFS itself is not a text of a filesystem: the
     * first mounted one may know nothing about it and would translate it
     * into its own "I/O error" (ADR 0009). */
    if (error == KERR_NOTSUP) {
        return "operation not supported";
    }

    for (i = 0; i < g_count; i++) {
        if (g_mounts[i].ops->strerror != NULL) {
            return g_mounts[i].ops->strerror(error);
        }
    }

    switch (error) {
    case KERR_NONE:
        return "no error";
    case KERR_PERM:
        return "operation not permitted";
    case KERR_NOENT:
        return "no such file or directory";
    case KERR_EXIST:
        return "file exists";
    case KERR_INVAL:
        return "invalid argument";
    case KERR_NOTSUP:
        return "operation not supported";
    case KERR_NOTDIR:
        return "not a directory";
    case KERR_ISDIR:
        return "is a directory";
    case KERR_NOSPC:
        return "no space left on device";
    default:
        return "unknown error";
    }
}
