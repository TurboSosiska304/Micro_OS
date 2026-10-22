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

/* The flags of the open files, they follow the POSIX ones. */
#define VFS_O_RDONLY 0x0000
#define VFS_O_WRONLY 0x0001
#define VFS_O_RDWR 0x0002
#define VFS_O_CREAT 0x0100
#define VFS_O_TRUNC 0x0200
#define VFS_O_APPEND 0x0400

/* One entry of a directory. */
struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    k_u32_t inode;
    k_u8_t type; /* 1: a file, 2: a directory */
    k_u64_t size;
};

/* The information about an object. */
struct vfs_stat {
    k_u32_t inode;
    k_u8_t type;
    k_u64_t size;
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
    int (*stat)(void *ctx, const char *path, struct vfs_stat *out);
    int (*mkdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
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
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_statfs(const char *path, struct vfs_statfs *out);

/* The message of the last error. */
const char *vfs_strerror(int error);

#endif /* MICRO_OS_KERNEL_VFS_H */
