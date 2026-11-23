/*
 * Micro OS — the two moments of a file, for a portable source.
 *
 * `utime` is the call of a machine whose filesystem lets a program set the
 * access and modification time of a file. The filesystem of this system keeps
 * the moment of a change (`docs/memory.md`, ADR 0013) and no caller sets it,
 * so the port answers the success of a copy: the bytes are the ones of the
 * source, and the moment of the record is the one of the write.
 */
#ifndef MICRO_OS_ZSTD_PORT_UTIME_H
#define MICRO_OS_ZSTD_PORT_UTIME_H

#include <sys/stat.h>

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char *path, const struct utimbuf *times);

#endif /* MICRO_OS_ZSTD_PORT_UTIME_H */
