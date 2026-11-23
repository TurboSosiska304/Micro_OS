/*
 * Micro OS — the port layer of SQLite: a VFS on the calls of this system.
 *
 * SQLite is built with SQLITE_OS_OTHER=1, so the library asks for no POSIX
 * kernel of somebody else: this file is its whole operating system. The VFS
 * answers with the calls of `components/libc/user` on the guest (x86-64) and
 * with the libc of the host in the unit tests. Both are the same code: only
 * the names of the entry points and the shape of an error differ, and the
 * wrappers below give the one shape SQLite expects - an SQLITE_* code.
 *
 * The machine is one:
 *
 *  - the locks are no-ops. One task of one machine has the file; there is no
 *    second process to keep out, and the journal of SQLite is enough for a
 *    transaction that fails. The VFS still remembers the level so that
 *    SQLITE_FCNTL_LOCKSTATE answers the truth;
 *  - the clock is `uptime`, the milliseconds since the boot: xCurrentTime
 *    and xCurrentTimeInt64 answer the moment of the boot and not a wall
 *    clock, which the machine does not have (docs/debts.md);
 *  - there is no fsync in the ABI and no truncate. xSync is a no-op because
 *    MicroFS hands every write to the medium in the call; xTruncate copies
 *    the first bytes of the file to a temporary name and renames it over the
 *    original, which is the same bytes and one more pass over the file
 *    (docs/debts.md);
 *  - there is no mmap and no shared memory: SQLITE_OMIT_WAL is set and the
 *    methods of a shared-memory file do not exist, so the journal is the
 *    rollback one (DELETE, PERSIST, TRUNCATE or MEMORY - the choice of the
 *    SQL statement, not of this file);
 *  - xDeviceCharacteristics is 0: a sector of the medium is not atomic and
 *    no write of this VFS happens in an order the filesystem promises.
 *
 * The port is part of the library of the guest (third_party/sqlite/port) and
 * of the host test (tests/cases/test_sqlite.c) with SQLITE_PORT_HOST.
 */
#include "sqlite3.h"

#ifdef SQLITE_PORT_HOST

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#else /* the guest */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ulibc.h"

#endif

#include "sqlite_port.h"

/* The moment of the boot in milliseconds. The host has a real clock, and the
 * test is the same code over it: a database of the host carries the date of
 * the host and a database of the guest the seconds of the boot. */
static sqlite3_int64 mx_now_ms(void)
{
#ifdef SQLITE_PORT_HOST
    struct timeval now;

    (void)gettimeofday(&now, NULL);
    return (sqlite3_int64)now.tv_sec * 1000 + (sqlite3_int64)(now.tv_usec / 1000);
#else
    return (sqlite3_int64)uptime();
#endif
}

/*
 * The calls of the library answer two shapes: `open` and `access` are the
 * POSIX shape (-1 and errno), `read`, `write`, `lseek` and `close` are the
 * raw answer of the ABI (a negative number with the error). This wrapper
 * turns both into -1 and errno, which is what the VFS reads.
 */
static int mx_failed(long answer)
{
    if (answer < 0) {
        if (errno == 0) {
            errno = (int)-answer;
        }
        return 1;
    }

    return 0;
}

struct mx_file {
    sqlite3_file base;
    int fd;
    int flags;       /* the SQLITE_OPEN_* flags of xOpen */
    int lock;        /* the level of SQLITE_LOCK_*, for FCNTL_LOCKSTATE */
    int delete_close;/* SQLITE_OPEN_DELETEONCLOSE */
    char path[SQLITE_MICROOS_PATH_MAX];
};

static int mx_close(sqlite3_file *file)
{
    struct mx_file *mine = (struct mx_file *)file;
    int rc = SQLITE_OK;

    if (mine->fd >= 0) {
        if (mx_failed(close(mine->fd))) {
            rc = SQLITE_IOERR_CLOSE;
        }
        mine->fd = -1;
    }

    if (mine->delete_close && mine->path[0] != '\0') {
        if (mx_failed(unlink(mine->path)) && errno != ENOENT) {
            rc = SQLITE_IOERR_DELETE;
        }
    }

    return rc;
}

static int mx_read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset)
{
    struct mx_file *mine = (struct mx_file *)file;
    char *out = (char *)buffer;
    int got = 0;

    if (amount < 0 || offset < 0) {
        return SQLITE_IOERR_READ;
    }

    if (mx_failed(lseek(mine->fd, (long)offset, SEEK_SET))) {
        return SQLITE_IOERR_READ;
    }

    while (got < amount) {
        long count = read(mine->fd, out + got, (size_t)(amount - got));

        if (count == 0) {
            break; /* the end of the file */
        }
        if (mx_failed(count)) {
            return SQLITE_IOERR_READ;
        }
        got += (int)count;
    }

    if (got < amount) {
        /* A short read is not an error of the medium: SQLite asks for a page
         * the file does not have yet and expects the rest of the buffer to be
         * zero. */
        memset(out + got, 0, (size_t)(amount - got));
        return SQLITE_IOERR_SHORT_READ;
    }

    return SQLITE_OK;
}

static int mx_write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset)
{
    struct mx_file *mine = (struct mx_file *)file;
    const char *in = (const char *)buffer;
    int wrote = 0;

    if (amount < 0 || offset < 0) {
        return SQLITE_IOERR_WRITE;
    }

    if (mx_failed(lseek(mine->fd, (long)offset, SEEK_SET))) {
        return SQLITE_IOERR_WRITE;
    }

    while (wrote < amount) {
        long count = write(mine->fd, in + wrote, (size_t)(amount - wrote));

        if (mx_failed(count) || count == 0) {
            return SQLITE_IOERR_WRITE;
        }
        wrote += (int)count;
    }

    return SQLITE_OK;
}

#ifndef SQLITE_PORT_HOST
/*
 * The ABI has no ftruncate. The bytes of the file are copied to a name beside
 * it and the copy is renamed over the original; the caller's descriptor is
 * reopened on the same flags. It is a pass over the file the kernel makes
 * twice, and the reason the journal of the program of the guest lives in
 * memory (docs/debts.md).
 */
static int mx_truncate_copy(struct mx_file *mine, sqlite3_int64 size)
{
    char temp[SQLITE_MICROOS_PATH_MAX];
    sqlite3_int64 have;
    sqlite3_int64 left = size;
    char block[1024];
    int temp_fd;
    long count;

    if (strlen(mine->path) + 6u >= sizeof(temp)) {
        return SQLITE_IOERR_TRUNCATE;
    }
    snprintf(temp, sizeof(temp), "%s.trunc", mine->path);

    if (mx_failed(lseek(mine->fd, 0, SEEK_END))) {
        return SQLITE_IOERR_TRUNCATE;
    }
    have = (sqlite3_int64)lseek(mine->fd, 0, SEEK_CUR);
    if (have < 0) {
        return SQLITE_IOERR_TRUNCATE;
    }

    temp_fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (temp_fd < 0) {
        return SQLITE_IOERR_TRUNCATE;
    }

    if (mx_failed(lseek(mine->fd, 0, SEEK_SET))) {
        close(temp_fd);
        unlink(temp);
        return SQLITE_IOERR_TRUNCATE;
    }

    while (left > 0) {
        int want = (left > (sqlite3_int64)sizeof(block)) ? (int)sizeof(block) : (int)left;
        int got = 0;

        while (got < want) {
            count = read(mine->fd, block + got, (size_t)(want - got));
            if (count == 0) {
                break;
            }
            if (mx_failed(count)) {
                close(temp_fd);
                unlink(temp);
                return SQLITE_IOERR_TRUNCATE;
            }
            got += (int)count;
        }

        if (got < want) {
            memset(block + got, 0, (size_t)(want - got));
        }

        if (mx_failed(write(temp_fd, block, (size_t)want))) {
            close(temp_fd);
            unlink(temp);
            return SQLITE_IOERR_TRUNCATE;
        }

        left -= want;
    }

    if (mx_failed(close(temp_fd))) {
        unlink(temp);
        return SQLITE_IOERR_TRUNCATE;
    }

    if (mx_failed(close(mine->fd))) {
        unlink(temp);
        mine->fd = -1;
        return SQLITE_IOERR_TRUNCATE;
    }
    mine->fd = -1;

    if (mx_failed(rename(temp, mine->path))) {
        unlink(temp);
        return SQLITE_IOERR_TRUNCATE;
    }

    mine->fd = open(mine->path, (mine->flags & SQLITE_OPEN_READWRITE) ? O_RDWR : O_RDONLY);
    if (mine->fd < 0) {
        return SQLITE_IOERR_TRUNCATE;
    }

    return SQLITE_OK;
}
#endif

static int mx_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    struct mx_file *mine = (struct mx_file *)file;

    if (size < 0) {
        return SQLITE_IOERR_TRUNCATE;
    }

#ifdef SQLITE_PORT_HOST
    if (ftruncate(mine->fd, (off_t)size) != 0) {
        return SQLITE_IOERR_TRUNCATE;
    }

    return SQLITE_OK;
#else
    return mx_truncate_copy(mine, size);
#endif
}

static int mx_sync(sqlite3_file *file, int flags)
{
    (void)file;
    (void)flags;

    /*
     * MicroFS writes a block to the medium in the call and the ABI has no
     * flush; the host test asks its own library for one so that the test is
     * the same promise the SQLite of a Unix-like system makes.
     */
#ifdef SQLITE_PORT_HOST
    {
        struct mx_file *mine = (struct mx_file *)file;

        if (fsync(mine->fd) != 0) {
            return SQLITE_IOERR_FSYNC;
        }
    }
#endif

    return SQLITE_OK;
}

static int mx_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
    struct mx_file *mine = (struct mx_file *)file;
    long end;

    if (mx_failed(lseek(mine->fd, 0, SEEK_END))) {
        return SQLITE_IOERR_FSTAT;
    }

    end = lseek(mine->fd, 0, SEEK_CUR);
    if (end < 0) {
        return SQLITE_IOERR_FSTAT;
    }

    *size = (sqlite3_int64)end;
    return SQLITE_OK;
}

/*
 * One machine, no second process: a lock is a number this file remembers.
 * The answer is SQLITE_OK, so SQLite believes the lock is held, and the
 * journal of the transaction is what corrects a failure. This is a decision
 * of the port and not a missing piece (docs/debts.md).
 */
static int mx_lock(sqlite3_file *file, int level)
{
    struct mx_file *mine = (struct mx_file *)file;

    mine->lock = level;
    return SQLITE_OK;
}

static int mx_unlock(sqlite3_file *file, int level)
{
    struct mx_file *mine = (struct mx_file *)file;

    mine->lock = level;
    return SQLITE_OK;
}

static int mx_check_reserved_lock(sqlite3_file *file, int *reserved)
{
    (void)file;

    *reserved = 0; /* nobody else is on this machine */
    return SQLITE_OK;
}

static int mx_file_control(sqlite3_file *file, int op, void *argument)
{
    struct mx_file *mine = (struct mx_file *)file;

    switch (op) {
    case SQLITE_FCNTL_LOCKSTATE:
        *(int *)argument = mine->lock;
        return SQLITE_OK;
    case SQLITE_FCNTL_SIZE_HINT:
    case SQLITE_FCNTL_CHUNK_SIZE:
    case SQLITE_FCNTL_SYNC_OMITTED:
        return SQLITE_OK;
    default:
        return SQLITE_NOTFOUND;
    }
}

static int mx_sector_size(sqlite3_file *file)
{
    (void)file;

    return 512; /* the sector of the medium of this machine (ADR 0022) */
}

static int mx_device_characteristics(sqlite3_file *file)
{
    (void)file;

    return 0;
}

static const sqlite3_io_methods mx_io_methods = {
    .iVersion = 1, /* no shared memory, no fetch: see SQLITE_OMIT_WAL */
    .xClose = mx_close,
    .xRead = mx_read,
    .xWrite = mx_write,
    .xTruncate = mx_truncate,
    .xSync = mx_sync,
    .xFileSize = mx_file_size,
    .xLock = mx_lock,
    .xUnlock = mx_unlock,
    .xCheckReservedLock = mx_check_reserved_lock,
    .xFileControl = mx_file_control,
    .xSectorSize = mx_sector_size,
    .xDeviceCharacteristics = mx_device_characteristics,
};

static int mx_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *file, int flags,
                   int *out_flags)
{
    struct mx_file *mine = (struct mx_file *)file;
    int oflags = 0;
    int fd;

    (void)vfs;

    memset(mine, 0, sizeof(*mine));
    mine->fd = -1;

    if (name == NULL || name[0] == '\0') {
        return SQLITE_CANTOPEN;
    }

    /*
     * The master journal is the two-phase commit of several databases at
     * once; this machine has one volume and the port does not promise the
     * atomicity of a transaction of two files.
     */
    if ((flags & SQLITE_OPEN_MASTER_JOURNAL) != 0) {
        return SQLITE_CANTOPEN;
    }

    if ((flags & SQLITE_OPEN_READWRITE) != 0) {
        oflags = O_RDWR;
    } else {
        oflags = O_RDONLY;
    }

    if ((flags & SQLITE_OPEN_CREATE) != 0) {
        oflags |= O_CREAT;
    }

    if (strlen(name) >= sizeof(mine->path)) {
        return SQLITE_CANTOPEN;
    }

    fd = open(name, oflags, 0644);
    if (fd < 0) {
        return ((errno == ENOENT) ? SQLITE_CANTOPEN : SQLITE_IOERR);
    }

    mine->fd = fd;
    mine->flags = flags;
    mine->lock = SQLITE_LOCK_NONE;
    mine->delete_close = ((flags & SQLITE_OPEN_DELETEONCLOSE) != 0);
    memcpy(mine->path, name, strlen(name) + 1u);
    mine->base.pMethods = &mx_io_methods;

    if (out_flags != NULL) {
        *out_flags = flags & (SQLITE_OPEN_READONLY | SQLITE_OPEN_READWRITE);
    }

    return SQLITE_OK;
}

static int mx_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    (void)vfs;
    (void)sync_dir;

    if (mx_failed(unlink(name))) {
        return (errno == ENOENT) ? SQLITE_OK : SQLITE_IOERR_DELETE;
    }

    return SQLITE_OK;
}

static int mx_access(sqlite3_vfs *vfs, const char *name, int flags, int *answer)
{
    struct stat info;

    (void)vfs;

    if (stat(name, &info) != 0) {
        *answer = 0;
        return SQLITE_OK;
    }

    if (flags == SQLITE_ACCESS_READWRITE) {
        *answer = (access(name, W_OK) == 0) ? 1 : 0;
        return SQLITE_OK;
    }

    *answer = 1;
    return SQLITE_OK;
}

static int mx_full_pathname(sqlite3_vfs *vfs, const char *name, int size, char *out)
{
    char cwd[SQLITE_MICROOS_PATH_MAX];
    size_t length;

    (void)vfs;

    if (name == NULL || out == NULL || size <= 0) {
        return SQLITE_CANTOPEN;
    }

    if (name[0] == '/') {
        length = strlen(name);
        if (length + 1u > (size_t)size) {
            return SQLITE_CANTOPEN;
        }
        memcpy(out, name, length + 1u);
        return SQLITE_OK;
    }

    /* A relative name is joined to the directory of the program, the same
     * rule the library of a program follows for every path (`path.c`). */
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return SQLITE_CANTOPEN;
    }

    length = strlen(cwd);
    if (length + 1u + strlen(name) + 1u > (size_t)size) {
        return SQLITE_CANTOPEN;
    }

    memcpy(out, cwd, length);
    if (length == 0 || out[length - 1u] != '/') {
        out[length++] = '/';
    }
    memcpy(out + length, name, strlen(name) + 1u);

    return SQLITE_OK;
}

/* [SQLITE_OS_OTHER] The dynamic loader is omitted from the build; the four
 * methods are here for the shape of the structure and never called. */
static void *mx_dl_open(sqlite3_vfs *vfs, const char *name)
{
    (void)vfs;
    (void)name;
    return NULL;
}

static void mx_dl_error(sqlite3_vfs *vfs, int size, char *message)
{
    (void)vfs;
    (void)size;

    if (message != NULL) {
        message[0] = '\0';
    }
}

static void (*mx_dl_sym(sqlite3_vfs *vfs, void *handle, const char *name))(void)
{
    (void)vfs;
    (void)handle;
    (void)name;
    return NULL;
}

static void mx_dl_close(sqlite3_vfs *vfs, void *handle)
{
    (void)vfs;
    (void)handle;
}

/*
 * The bytes of `randomblob()` and of a temporary name. There is no entropy
 * device on this machine: the generator is a linear one seeded from the
 * clock of the boot, the pid and the address of the VFS. It is not a
 * cryptographic source and the port does not pretend it is (`docs/debts.md`).
 */
static int mx_randomness(sqlite3_vfs *vfs, int amount, char *out)
{
    static sqlite3_uint64 state;
    int index;

    if (state == 0) {
        state = (sqlite3_uint64)mx_now_ms() ^ ((sqlite3_uint64)getpid() << 32) ^
                (sqlite3_uint64)(size_t)vfs ^ 0x9e3779b97f4a7c15ull;
        if (state == 0) {
            state = 1;
        }
    }

    for (index = 0; index < amount; index++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[index] = (char)(state >> 24);
    }

    return amount;
}

static int mx_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;

    if (microseconds <= 0) {
        return 0;
    }

    return usleep((unsigned)microseconds);
}

/*
 * The moment of the boot as a Julian day, which is what SQLite keeps. The
 * machine has no wall clock: the day is the 1st of January 1970 and the time
 * of the day is the uptime (docs/debts.md). xCurrentTimeInt64 answers the
 * same moment in milliseconds.
 */
#define MX_UNIX_TO_JD_MS 210866760000000ll /* the Julian day of 1970-01-01 */

static int mx_current_time(sqlite3_vfs *vfs, double *moment)
{
    (void)vfs;

    *moment = (double)(mx_now_ms() + MX_UNIX_TO_JD_MS) / 86400000.0;
    return SQLITE_OK;
}

static int mx_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *moment)
{
    (void)vfs;

    *moment = mx_now_ms() + MX_UNIX_TO_JD_MS;
    return SQLITE_OK;
}

static int mx_last_error(sqlite3_vfs *vfs, int size, char *message)
{
    const char *text;
    size_t length;

    (void)vfs;

    if (message == NULL || size <= 0) {
        return 0;
    }

    text = strerror(errno);
    length = strlen(text);
    if (length >= (size_t)size) {
        length = (size_t)size - 1u;
    }
    memcpy(message, text, length);
    message[length] = '\0';

    return (int)length;
}

static sqlite3_vfs mx_vfs = {
    .iVersion = 2, /* xCurrentTimeInt64 is there */
    .szOsFile = sizeof(struct mx_file),
    .mxPathname = SQLITE_MICROOS_PATH_MAX,
    .zName = SQLITE_MICROOS_VFS,
    .xOpen = mx_open,
    .xDelete = mx_delete,
    .xAccess = mx_access,
    .xFullPathname = mx_full_pathname,
    .xDlOpen = mx_dl_open,
    .xDlError = mx_dl_error,
    .xDlSym = mx_dl_sym,
    .xDlClose = mx_dl_close,
    .xRandomness = mx_randomness,
    .xSleep = mx_sleep,
    .xCurrentTime = mx_current_time,
    .xGetLastError = mx_last_error,
    .xCurrentTimeInt64 = mx_current_time_int64,
};

int sqlite3_os_init(void)
{
    return sqlite3_vfs_register(&mx_vfs, 1);
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
