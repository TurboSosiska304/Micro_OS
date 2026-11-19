/*
 * Micro OS — the volume of the simulator as a block device.
 *
 * MicroFS is a consumer of blocks (kernel/blockdev.h): on the board the
 * blocks are a partition of the flash, here they are a file of the machine.
 * The same filesystem code, the same layout, the same scripts of /etc - the
 * only difference is where a sector is kept, which is what the host port is
 * for.
 *
 * The file is created on the first access and filled with 0xFF, the state of
 * an erased flash. MicroFS formats a medium that was never written and only
 * then, so an erased file is what makes the first boot of the simulator look
 * like the first boot of a fresh board. A file of another size is not a
 * volume of this device any more: it is erased the same way instead of
 * mounting whatever its first sectors happen to say.
 */
#include "port_host.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/blockdev.h"
#include "kernel/log.h"
#include "kernel/types.h"

/* The environment that points the volume at another file. */
#define VOLUME_ENV "MICROOS_VOLUME"
#define VOLUME_DEFAULT "/tmp/microos-sim.img"

/* The name of the device in the registry of the kernel. */
#define VOLUME_NAME "file0"

/* The sector of the device in bytes. A sector is one block of MicroFS. */
#define VOLUME_SECTOR_BYTES ((off_t)PORT_HOST_VOLUME_SECTOR_SIZE)

/*
 * The volume is opened on the first sector that is asked for, and the file
 * descriptor is shared by every task that reads or writes: the lock covers
 * the opening, the operations themselves go through pread/pwrite and do not
 * touch the offset of the descriptor.
 */
static pthread_mutex_t s_open_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fd = -1;

const char *port_host_volume_path(void)
{
    const char *path = getenv(VOLUME_ENV);

    return (path != NULL && path[0] != '\0') ? path : VOLUME_DEFAULT;
}

static off_t volume_bytes(void)
{
    return (off_t)MICROOS_VOLUME_SECTORS * VOLUME_SECTOR_BYTES;
}

static bool volume_in_range(k_u32_t sector, k_size_t count)
{
    if (count == 0 || sector >= MICROOS_VOLUME_SECTORS) {
        return false;
    }

    return count <= (k_size_t)MICROOS_VOLUME_SECTORS - sector;
}

/* Write 0xFF over the whole file: the medium comes erased, as the flash does. */
static int volume_erase_all(void)
{
    k_u8_t blank[PORT_HOST_VOLUME_SECTOR_SIZE];
    off_t offset;

    memset(blank, 0xFF, sizeof(blank));

    if (ftruncate(s_fd, 0) != 0 || ftruncate(s_fd, volume_bytes()) != 0) {
        return -1;
    }

    for (offset = 0; offset < volume_bytes(); offset += VOLUME_SECTOR_BYTES) {
        if (pwrite(s_fd, blank, sizeof(blank), offset) != (ssize_t)sizeof(blank)) {
            return -1;
        }
    }

    return fsync(s_fd) == 0 ? 0 : -1;
}

static int volume_open_locked(void)
{
    struct stat info;

    s_fd = open(port_host_volume_path(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (s_fd < 0 && errno != EEXIST) {
        kerror("file0: cannot create %s: %s", port_host_volume_path(), strerror(errno));
        return -1;
    }

    if (s_fd < 0) {
        s_fd = open(port_host_volume_path(), O_RDWR);
        if (s_fd < 0) {
            kerror("file0: cannot open %s: %s", port_host_volume_path(), strerror(errno));
            return -1;
        }
    }

    if (fstat(s_fd, &info) != 0) {
        kerror("file0: cannot read the size of %s: %s", port_host_volume_path(), strerror(errno));
        close(s_fd);
        s_fd = -1;
        return -1;
    }

    /* The size of the file is the shape of the volume: another size is not
     * the volume this device describes, so it starts from an erased medium. */
    if (info.st_size != volume_bytes()) {
        if (volume_erase_all() != 0) {
            kerror("file0: cannot prepare %s: %s", port_host_volume_path(), strerror(errno));
            close(s_fd);
            s_fd = -1;
            return -1;
        }
        kinfo("file0: %s is prepared, %u KB", port_host_volume_path(),
              (unsigned)(volume_bytes() / 1024));
    }

    return 0;
}

static int volume_open(void)
{
    int rc = 0;

    pthread_mutex_lock(&s_open_lock);

    if (s_fd < 0) {
        rc = volume_open_locked();
    }

    pthread_mutex_unlock(&s_open_lock);

    return rc;
}

/* A whole range of a regular file: a short read or write is an error here. */
static int volume_xfer(void *buf, size_t len, off_t offset, bool writing)
{
    k_u8_t *bytes = (k_u8_t *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = writing ? pwrite(s_fd, bytes + done, len - done, offset + (off_t)done)
                            : pread(s_fd, bytes + done, len - done, offset + (off_t)done);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            /* The file is shorter than the range: the volume is not there. */
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }

    return 0;
}

static int volume_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    off_t offset = (off_t)sector * VOLUME_SECTOR_BYTES;
    size_t len = (size_t)count * (size_t)VOLUME_SECTOR_BYTES;

    (void)ctx;

    if (buf == NULL || !volume_in_range(sector, count)) {
        return -1;
    }

    if (volume_open() != 0) {
        return -1;
    }

    if (volume_xfer(buf, len, offset, false) != 0) {
        kerror("file0: cannot read %u sector(s) at %u: %s", (unsigned)count, (unsigned)sector,
               strerror(errno));
        return -1;
    }

    return 0;
}

static int volume_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    off_t offset = (off_t)sector * VOLUME_SECTOR_BYTES;
    size_t len = (size_t)count * (size_t)VOLUME_SECTOR_BYTES;

    (void)ctx;

    if (buf == NULL || !volume_in_range(sector, count)) {
        return -1;
    }

    if (volume_open() != 0) {
        return -1;
    }

    if (volume_xfer((void *)buf, len, offset, true) != 0) {
        kerror("file0: cannot write %u sector(s) at %u: %s", (unsigned)count, (unsigned)sector,
               strerror(errno));
        return -1;
    }

    return 0;
}

static int volume_sync(void *ctx)
{
    (void)ctx;

    if (s_fd < 0) {
        return 0;
    }

    if (fsync(s_fd) != 0) {
        kerror("file0: cannot flush %s: %s", port_host_volume_path(), strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * A file needs no erase: a write of a sector replaces its bytes, while the
 * flash can only clear bits and has to be erased first. The op stays NULL,
 * and the filesystem writes the way it writes to a medium that accepts it.
 */
static const struct k_blockdev_ops s_ops = {
    .read = volume_read,
    .write = volume_write,
    .erase = NULL,
    .sync = volume_sync,
};

/* The device lives as long as the process: the kernel keeps the pointer. */
static struct k_blockdev s_device;

void port_host_register_blockdev(void)
{
    s_device.name = VOLUME_NAME;
    s_device.sector_size = PORT_HOST_VOLUME_SECTOR_SIZE;
    s_device.sectors = MICROOS_VOLUME_SECTORS;
    s_device.ops = &s_ops;
    s_device.ctx = NULL;

    if (k_blockdev_register(&s_device) != 0) {
        kerror("file0: the device is not registered");
        return;
    }

    kinfo("file0: %u sector(s) of %u bytes on %s", (unsigned)s_device.sectors,
          (unsigned)s_device.sector_size, port_host_volume_path());
}
