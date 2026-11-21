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
    int (*mkdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
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

/* Mount a filesystem at a point of the tree. */
int vfs_mount(const char *point, const struct vfs_operations *ops, void *ctx);

/* Unmount a filesystem. */
int vfs_umount(const char *point);

/* The number of the mounted filesystems. */
k_size_t vfs_mount_count(void);

/* The name of a mounted filesystem. */
const char *vfs_mount_name(k_size_t index);

/* The operations of a filesystem, for the shell. */
int vfs_open(const char *path, int flags, struct vfs_file *out, void *priv);
int vfs_read(struct vfs_file *file, void *buf, k_size_t size);
int vfs_write(struct vfs_file *file, const void *buf, k_size_t size);
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
int vfs_statfs(const char *path, struct vfs_statfs *out);

/* The mode and the owner of an object; a filesystem without them says so. */
int vfs_chmod(const char *path, k_u32_t mode);
int vfs_chown(const char *path, k_u32_t owner);

/* The message of the last error. */
const char *vfs_strerror(int error);

#endif /* MICRO_OS_KERNEL_VFS_H */
