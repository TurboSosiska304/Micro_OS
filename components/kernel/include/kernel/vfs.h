/*
 * Micro OS — the virtual filesystem.
 *
 * The filesystems are mounted at a point of the tree of the directories and
 * the paths are resolved by the longest matching prefix, like in Linux.
 */
#ifndef MICRO_OS_KERNEL_VFS_H
#define MICRO_OS_KERNEL_VFS_H

#include "kernel/types.h"

#define VFS_MAX_MOUNTS 4
#define VFS_MAX_TYPES 4
#define VFS_TYPE_NAME_MAX 16
#define VFS_NAME_MAX 32
#define VFS_PATH_MAX 128

/*
 * The one user of the system (ADR 0005). The VFS checks the search bit of
 * the directories of a path for that uid and grants it nothing extra.
 */
#define VFS_UID 0u

/* The flags of the open files, they follow the POSIX ones. */
#define VFS_O_RDONLY 0x0000
#define VFS_O_WRONLY 0x0001
#define VFS_O_RDWR 0x0002
#define VFS_O_CREAT 0x0100
#define VFS_O_TRUNC 0x0200
#define VFS_O_APPEND 0x0400
/*
 * The open of a directory, which is what a descriptor of `readdir` is. The
 * value is not the one of Linux on purpose: the flags of this contract are
 * one small byte and every one of them fits there.
 */
#define VFS_O_DIRECTORY 0x0800

/* One entry of a directory. */
struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    k_u32_t inode;
    k_u8_t type; /* 1: a file, 2: a directory */
    k_u64_t size;
};

/*
 * The information about an object. The mode and the owner are the numbers
 * the filesystem keeps for the object (ADR 0005): nine bits of rwx for the
 * user, the group and the others, and a uid. A filesystem that has no modes
 * of its own - procfs, devfs - answers with a zero, and a zero means
 * exactly that: no modes, not a file nobody may touch.
 */
struct vfs_stat {
    k_u32_t inode;
    k_u8_t type;
    k_u64_t size;
    k_u32_t mode;
    k_u32_t owner;
};

/*
 * The information about an object with the moments of a change (ADR 0013).
 * The two times are the seconds of Unix, 64 bits wide so that the year 2038
 * is not a wall this filesystem hits; zero means "this filesystem keeps no
 * time" - a volume of MicroFS of version 1, or a filesystem like procfs that
 * has no moments of its own.
 *
 * The field of the access time is deliberately absent: a read does not
 * change a record in this system, and a field that is always zero would
 * promise a moment that nothing keeps.
 */
struct vfs_statx {
    k_u32_t inode;
    k_u8_t type;
    k_u64_t size;
    k_u32_t mode;
    k_u32_t owner;
    k_u64_t mtime; /* the last change of the content */
    k_u64_t ctime; /* the last change of the record, the mode or the owner */
};

/* The operations of a filesystem. */
struct vfs_file;

/* The information about the volume of a filesystem. */
struct vfs_statfs {
    k_u64_t total_blocks;
    k_u64_t free_blocks;
    k_u64_t total_inodes;
    k_u64_t free_inodes;
    k_u32_t block_size;
};

struct vfs_operations {
    int (*open)(void *ctx, const char *path, int flags, struct vfs_file *out);
    int (*read)(void *ctx, struct vfs_file *file, void *buf, k_size_t size);
    int (*write)(void *ctx, struct vfs_file *file, const void *buf, k_size_t size);
    /*
     * A control command of an open file (ADR 0028). The request and its
     * argument are the numbers of the ABI; a filesystem that has no command
     * of its own leaves the field empty and the caller learns KERR_INVAL. A
     * device that offers `/dev/fb0` or `/dev/input0` is the caller of this
     * operation and not the VFS: the meaning of a command belongs to the
     * device.
     */
    int (*ioctl)(void *ctx, struct vfs_file *file, k_u32_t request, k_u64_t arg);
    /*
     * Cut an open file to `size` bytes, the operation of `ftruncate`: a
     * shorter file gives its tail back, a longer one reads zeros there, and
     * the position of the descriptor is not moved. The call works on the
     * open file and not on a path because the descriptor already names the
     * object, and a name may have been removed or replaced since the open.
     *
     * A filesystem that cannot cut a file leaves the field empty and the
     * caller learns KERR_NOTSUP, which is what FAT and the virtual
     * filesystems answer; MicroFS and ext2 cut a file to any size.
     */
    int (*truncate)(void *ctx, struct vfs_file *file, k_u64_t size);
    int (*close)(void *ctx, struct vfs_file *file);
    int (*list)(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count);
    /*
     * The next entries of an open directory. The position lives in the
     * descriptor, so a second call continues where the first one stopped.
     * A filesystem without directories leaves the field empty.
     */
    int (*readdir)(void *ctx, struct vfs_file *file, struct vfs_dirent *out, k_size_t max,
                   k_size_t *count);
    int (*stat)(void *ctx, const char *path, struct vfs_stat *out);
    /*
     * The same information with the moments of a change. A filesystem that
     * keeps no time leaves the field empty and the VFS answers with the
     * zeros of "unknown" from the stat call above; the call is separate so
     * that a reader of an old filesystem is not asked for a number it does
     * not have.
     */
    int (*statx)(void *ctx, const char *path, struct vfs_statx *out);
    int (*mkdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    /*
     * Rename an object of the volume (ADR 0024 §5). The atomic install is
     * the reason it exists: the bytes of a program are written beside it,
     * and one call makes the new file visible under the old name. A
     * filesystem that has no such operation leaves the field empty and the
     * caller learns KERR_NOTSUP, which is what an install says before it
     * falls back to the window that is not atomic.
     *
     * The two paths belong to the same volume: the VFS refuses a rename
     * that would leave it with KERR_XDEV, because copying the bytes between
     * two filesystems is a different operation with a different failure.
     */
    int (*rename)(void *ctx, const char *from, const char *to);
    /*
     * The mode and the owner of an object. They are two calls and not one
     * "set the attributes", because a filesystem may well be able to change
     * one and not the other, and a caller that changes one number must not
     * have to carry the other one through the door.
     */
    int (*chmod)(void *ctx, const char *path, k_u32_t mode);
    int (*chown)(void *ctx, const char *path, k_u32_t owner);
    int (*statfs)(void *ctx, struct vfs_statfs *out);
    const char *(*strerror)(int error);
    /*
     * The filesystem is leaving the tree. The context may be released here:
     * a filesystem that keeps a volume in a static slot needs the call, a
     * virtual one that owns nothing leaves the field empty. The call runs
     * before the mount is forgotten, so it still sees its point.
     */
    int (*umount)(void *ctx);
};

/* An open file. */
struct vfs_file {
    k_u32_t inode;
    k_u64_t size;
    k_u64_t offset;
    int flags;
    void *priv;                       /* the data of the filesystem */
    const struct vfs_operations *ops; /* the filesystem that opened it */
    void *fs;                         /* the context of the filesystem */
};

/*
 * The size of the private data of an open file. The buffer belongs to the
 * caller (the kernel gives it to every open file of every task), so that
 * two tasks never share the data of their files.
 */
#define VFS_FILE_DATA_SIZE 64

/*
 * A type of filesystem is a name and the call that mounts a volume of it.
 * The VFS keeps a small table of the types the image was built with and
 * knows nothing else about them: "fat32" and "mfs" are two strings of the
 * configuration, not two branches of the kernel.
 *
 * The call mounts the volume itself by calling vfs_mount(); the VFS then
 * has the operations and the context of the filesystem and the two are the
 * same for every volume of every type.
 */
typedef int (*vfs_mount_fn)(const char *point, const char *device);

struct vfs_fs_type {
    char name[VFS_TYPE_NAME_MAX + 1];
    vfs_mount_fn mount;
};

/*
 * Mount a filesystem at a point of the tree.
 *
 * The point is an absolute path of its own: it begins with '/', it carries
 * no trailing slash except for the root itself. The point need not exist in
 * the filesystem above it, and neither need its parent (ADR 0009); what is
 * already there has to be a directory. The errors are therefore:
 *
 *   KERR_INVAL    the point is not a path of that shape;
 *   KERR_NOTDIR   the parent, or the point itself, is not a directory;
 *   KERR_BUSY     another filesystem is mounted at the point already;
 *   KERR_NOSPC    the table of the mounts has no room for another one.
 *
 * A second mount of the very same operations and context is a re-mount: the
 * call keeps the entry and takes the new pair, which is how a filesystem
 * that keeps a volume in a static slot replaces it. A different filesystem
 * at the same point is refused instead of hidden behind the new one.
 */
int vfs_mount(const char *point, const struct vfs_operations *ops, void *ctx);

/*
 * Register or forget a type. A name that is already in the table keeps its
 * slot and takes the new call, so a port may register the same type again
 * after a re-initialisation. The name is copied; the caller keeps its own.
 */
int vfs_register_type(const char *name, vfs_mount_fn mount);
int vfs_unregister_type(const char *name);

/* The types of the system, for the shell and the tests. */
k_size_t vfs_type_count(void);
const char *vfs_type_name(k_size_t index);
/*
 * The slot of a type, or -1 when the image has no type of that name. The
 * shell asks before it mounts, so that "there is no type 'fat32'" names the
 * word a person typed and not the number of an error.
 */
int vfs_type_find(const char *name);

/*
 * Mount a volume of a registered type. An unknown name is KERR_INVAL: the
 * type is a part of the configuration and a boot that asks for one the
 * image does not have has a broken configuration, not a broken medium.
 *
 * The point is checked before the mount function of the type runs, so a
 * point that is busy or malformed costs no volume: the caller gets the same
 * errors as vfs_mount() and the type is not called.
 */
int vfs_mount_type(const char *point, const char *type, const char *device);

/*
 * Unmount a filesystem.
 *
 * KERR_NOENT means that nothing is mounted at the point, KERR_INVAL that
 * the point is not a path of the tree. A filesystem that lies below the
 * point is KERR_BUSY, because the directory the smaller mount hangs from
 * would leave the tree with it. The root is the exception: the mounts of
 * /proc and /dev do not live on the volume of `/`, so `/` may leave while
 * they stay (which is what `mkfs -y` does with the volume of the board).
 *
 * A filesystem that refuses to leave - its `umount` operation returns an
 * error, KERR_NOTSUP among them - stays mounted and its error is returned.
 */
int vfs_umount(const char *point);

/* The number of the mounted filesystems. */
k_size_t vfs_mount_count(void);

/* The name of a mounted filesystem. */
const char *vfs_mount_name(k_size_t index);

/* The table of the volumes mounted by the boot. */
#define VFS_FSTAB_PATH "/etc/fstab"

/* The longest line of the table; a longer one is reported and skipped. */
#define VFS_FSTAB_LINE_MAX 128

/*
 * Mount every volume of a table of the form of /etc/fstab: one line of
 * `device point type [options]` per volume, '#' starts a comment and an
 * empty line is not a line at all.
 *
 * The table is read through the VFS in the current task, so it has to live
 * in a mounted filesystem (MicroFS has /etc). No memory is allocated: the
 * lines are assembled in a buffer of the stack.
 *
 * Returns the number of the volumes mounted, or a negative error when the
 * file cannot be opened or read. A line that does not mount - few fields, a
 * type the image does not have, an unknown option, a device that is not
 * there - is reported in the log and skipped: a broken line of the table
 * does not stop the boot. Only `defaults` is a known option for now; the
 * field is read so that a table of Linux does not have to be edited, and its
 * other words are refused instead of silently ignored.
 */
int vfs_mount_fstab(const char *path);

/* The operations of a filesystem, for the shell. */
int vfs_open(const char *path, int flags, struct vfs_file *out, void *priv);
int vfs_read(struct vfs_file *file, void *buf, k_size_t size);
int vfs_write(struct vfs_file *file, const void *buf, k_size_t size);
/*
 * A control command of an open file: the request and the argument are the
 * numbers the ABI names, and the filesystem that owns the file says what they
 * mean. A filesystem without a command answers KERR_INVAL, which is what the
 * call answered before the commands existed.
 */
int vfs_ioctl(struct vfs_file *file, k_u32_t request, k_u64_t arg);
/*
 * Cut an open file to `size` bytes (the `ftruncate` of POSIX). The
 * descriptor has to be open for writing, a directory is not a file that can
 * be cut, and a filesystem without the operation answers KERR_NOTSUP. A
 * size that the volume cannot hold is KERR_NOSPC; a size wider than the
 * format of the filesystem is its own refusal.
 */
int vfs_truncate(struct vfs_file *file, k_u64_t size);
int vfs_close(struct vfs_file *file);
int vfs_list(const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count);
/*
 * The entries of a directory that was opened with VFS_O_DIRECTORY. The
 * descriptor keeps the position; the end of the directory is an empty
 * answer and not an error. A descriptor that is not a directory is ENOTDIR.
 */
int vfs_readdir(struct vfs_file *file, struct vfs_dirent *out, k_size_t max, k_size_t *count);
int vfs_stat(const char *path, struct vfs_stat *out);
/*
 * The same object with its moments. A filesystem without a statx operation
 * is read through its stat operation and the two times are zero, which is
 * what "unknown" is in this contract.
 */
int vfs_statx(const char *path, struct vfs_statx *out);
/*
 * Create, remove or change an object. Every directory of the path is walked
 * and its `x` bit is checked first, exactly as for the calls above: the
 * parent of the object is the last directory that is passed through. The
 * object itself keeps its own check, and there is no bypass for the one
 * user of the system (ADR 0005).
 */
int vfs_mkdir(const char *path);

/*
 * Create a directory and every parent of it: /etc/init.d needs /etc first.
 * A directory that is already there is not an error.
 */
int vfs_mkdir_p(const char *path);
int vfs_unlink(const char *path);

/*
 * Rename an object inside one filesystem: the name `from` becomes `to`. An
 * object already called `to` is replaced when it is a file, the way rename
 * of POSIX does it (this is the atomic step of an install, ADR 0024 §5);
 * a directory at `to` is refused with KERR_ISDIR, because the tree below it
 * would be lost.
 *
 * The two paths have to be in the same mounted filesystem; a rename between
 * two of them is KERR_XDEV, because it is a copy with a different story of a
 * half-done failure. A filesystem without the operation answers KERR_NOTSUP
 * and the caller decides whether the weaker way is acceptable. A directory
 * is renamed only within its own parent: moving it would have to move the
 * tree below it, and no filesystem of this system does that yet.
 */
int vfs_rename(const char *from, const char *to);

int vfs_statfs(const char *path, struct vfs_statfs *out);

/* The mode and the owner of an object; a filesystem without them says so. */
int vfs_chmod(const char *path, k_u32_t mode);
int vfs_chown(const char *path, k_u32_t owner);

/* The message of the last error. */
const char *vfs_strerror(int error);

#endif /* MICRO_OS_KERNEL_VFS_H */
