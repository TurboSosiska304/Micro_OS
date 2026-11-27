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
 * The root of the system (ADR 0005). It is the uid of the boot and of the
 * kernel itself, the only number a task may not give up once it is another
 * user, and the only one the VFS lets through a triad it did not match. The
 * VFS compares the three triads of the mode with the uid of the task that
 * makes the call; a task without a record - the boot before the scheduler -
 * is root.
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
/*
 * The exclusive create: with VFS_O_CREAT the open fails with KERR_EXIST when
 * the name is already there instead of opening the object. A caller that
 * writes a file beside the old one and installs it with a rename asks for
 * this, because a name of somebody else must not be overwritten by a
 * half-written copy.
 */
#define VFS_O_EXCL 0x1000

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
 * user, the group and the others, a uid and a gid. A filesystem that has no
 * modes of its own - procfs, devfs - answers with a zero, and a zero means
 * exactly that: no modes, not a file nobody may touch. The same goes for the
 * group: a filesystem that keeps none answers zero, which is the group of
 * root, and no object of it is then ever "of the group" of a task that is
 * not root.
 */
struct vfs_stat {
    k_u32_t inode;
    k_u8_t type;
    k_u64_t size;
    k_u32_t mode;
    k_u32_t owner;
    /*
     * The group of the object (ADR 0044). It is a `k_u32_t` and not the
     * fifteen bits of a record because this is the question the check of the
     * access asks and not a field of a file format; the filesystems hold the
     * number they have. The order of the fields keeps the structure aligned
     * without a hole (a kernel structure, not the frozen ABI of statx).
     */
    k_u32_t group;
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
    /*
     * The group of the object (ADR 0038), in the room that the alignment of
     * `type` left free: the call is frozen (KSYS_STATX, ADR 0011) and an image
     * of 1.0 still runs on this kernel, so the answer keeps its 40 bytes and a
     * program of the release before this one reads the fields it knows. A
     * filesystem with no group - every one of this release but MicroFS, whose
     * version 3 records carry it - answers zero, which is the group of root.
     * The triad of the group in the check of the access is not read from this
     * number yet (docs/debts.md).
     */
    k_u16_t group;
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
    /*
     * Bring the data and the metadata of an open file to the medium, so that
     * a power cut after the call returns keeps the bytes the file had at the
     * call. A filesystem that cannot make that promise refuses with KERR_NOTSUP
     * instead of answering success for a write that may still be in a cache.
     */
    int (*fsync)(void *ctx, struct vfs_file *file);
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
     * Remove an empty directory. It is a call of its own and not the unlink
     * of a directory, because POSIX separates the two: unlink of a directory
     * answers KERR_ISDIR and rmdir of a file answers KERR_NOTDIR, so a
     * program that meant one cannot silently do the other. A directory that
     * still holds an entry is KERR_NOTEMPTY. A filesystem that has no
     * directories leaves the field empty.
     */
    int (*rmdir)(void *ctx, const char *path);
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
    /*
     * The group of an object. It is a call of its own for the same reason the
     * owner is: a filesystem may keep one and not the other. MicroFS keeps it
     * in the word of a version 3 record and ext2 in the `i_gid` of the inode;
     * FAT32 has no groups at all and leaves the field empty, so the caller
     * learns KERR_NOTSUP - "this volume has no such attribute" - rather than
     * a zero that would read back as the group of root.
     */
    int (*chgrp)(void *ctx, const char *path, k_u32_t group);
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
 *
 * Only root mounts a volume: a task of another uid learns KERR_PERM before
 * a type is looked up or a medium read. The boot calls this as root.
 */
int vfs_mount_type(const char *point, const char *type, const char *device);

/*
 * The mount that asks for the writes (ADR 0041 §2). A volume of a medium
 * with the door of the writes - the disks a controller found - is mounted
 * read-write only when the rule of kernel/volume.c allows it: the superblock
 * carries the mark of this system and `/etc/volumes` names the device. The
 * refusal is KERR_ROFS and the sentence of the rule is in the log; nothing
 * of the volume was read or written by the type.
 *
 * A medium without the door - the memory of the machine, the flash of the
 * board, the image of the tests - is mounted as vfs_mount_type() mounts it:
 * there is no rule to ask.
 */
int vfs_mount_type_write(const char *point, const char *type, const char *device);

/*
 * The second half of the mount of the root: the root is in the tree, so the
 * `/etc/volumes` of its own volume can be read, and the boot decides whether
 * the mount may write. A refusal (KERR_ROFS) leaves the mount in the tree and
 * read-only; a medium without the door is read-write, as every release
 * before the rule had it.
 */
int vfs_mount_root_writes(const char *point, const char *device);

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
 * Only root unmounts a volume; another uid learns KERR_PERM and nothing
 * leaves the tree.
 */
int vfs_umount(const char *point);

/* The number of the mounted filesystems. */
k_size_t vfs_mount_count(void);

/* The name of a mounted filesystem. */
const char *vfs_mount_name(k_size_t index);

/*
 * The volume behind a mount, the type that mounted it, and whether the mount
 * may write: the three answers of /proc/mounts, so that a volume mounted
 * read-write says `rw` and one the rule left alone says `ro` (ADR 0041 §2).
 * A mount a filesystem made for itself has no device and no type, and both
 * answers are NULL; such a mount is read-write.
 */
const char *vfs_mount_device(k_size_t index);
const char *vfs_mount_type_name(k_size_t index);
bool vfs_mount_is_read_only(k_size_t index);

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
/*
 * Bring the data and the metadata of an open file to the medium. A power cut
 * after the call returns KERR_NONE keeps the bytes the file had at the call;
 * a filesystem or a medium that cannot make that promise answers KERR_NOTSUP
 * and not a pretended success.
 */
int vfs_fsync(struct vfs_file *file);
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
 * Remove an empty directory. The object has to be a directory (KERR_NOTDIR
 * for a file) and it has to be empty (KERR_NOTEMPTY). The parent is walked
 * like the parent of unlink, so a directory that the caller cannot search is
 * not removed. A filesystem that cannot remove a directory answers
 * KERR_NOTSUP; the virtual filesystems leave the name of the operation empty
 * for that reason.
 */
int vfs_rmdir(const char *path);

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

/*
 * The bits of a mode that decide for the caller: the triad of the owner for
 * the owner of the object, the triad of the group for a task whose gid is the
 * group of the object, the triad of the others for everybody else, and all
 * three for root. An object of a filesystem that keeps no group answers zero,
 * which is the group of root, so a task that is not root never reads the
 * triad of the group of such an object.
 *
 * The same function answers the two doors of the question - the check of the
 * VFS (a write of a directory, the search of a path) and the check of the
 * edge of the system (the access of an open, the mark of a program) - so that
 * the two can never disagree about who may do what. The rule of root is the
 * rule of POSIX: it passes the read and the write; the execute bit is the
 * exception, which the caller of execve asks about itself.
 */
k_u32_t vfs_access_bits(const struct vfs_stat *info);

/*
 * The mode, the owner and the group of an object; a filesystem without them
 * says so.
 *
 * The three calls carry the policy of the system above the filesystem: chmod
 * is the owner of the object or root, chown is root alone, and chgrp is root
 * or the owner of the object giving it to the group it belongs to itself. A
 * caller that is none of those learns KERR_PERM. The rule lives here and not
 * at the edge of the system calls, because the shell and the tests reach the
 * same objects through these functions.
 *
 * The owner has this one widening over chown because a group gives away no
 * privilege: the owner already reads and writes its object, and the group it
 * may name is the group of its own task and no other. A task has one gid and
 * no supplementary list (ADR 0044), so "a group the caller belongs to" is
 * exactly that one number.
 */
int vfs_chmod(const char *path, k_u32_t mode);
int vfs_chown(const char *path, k_u32_t owner);
int vfs_chgrp(const char *path, k_u32_t group);

/* The message of the last error. */
const char *vfs_strerror(int error);

#endif /* MICRO_OS_KERNEL_VFS_H */
