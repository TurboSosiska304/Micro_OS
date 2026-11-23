/*
 * Micro OS — the settings of the SQLite port.
 *
 * The two numbers are the contract between the VFS (`sqlite_vfs.c`) and the
 * library: a path of this system is 127 bytes plus the terminator (VFS_PATH_MAX
 * of the kernel), and the name of the VFS is the one the shell of the program
 * asks `sqlite3_vfs_find()` for when a file names no VFS.
 */
#ifndef MICRO_OS_THIRD_PARTY_SQLITE_PORT_H
#define MICRO_OS_THIRD_PARTY_SQLITE_PORT_H

/* The 128 bytes of `VFS_PATH_MAX` in kernel/vfs.h. */
#define SQLITE_MICROOS_PATH_MAX 128

/* The name of the VFS in the table of SQLite. It is not the "unix" name: a
 * file of this machine is not a file of a Unix-like system, and a database
 * copied from one is read through this layer and not through the other. */
#define SQLITE_MICROOS_VFS "microos"

#endif /* MICRO_OS_THIRD_PARTY_SQLITE_PORT_H */
