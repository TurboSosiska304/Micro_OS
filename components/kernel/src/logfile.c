#include "kernel/logfile.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/vfs.h"

/* The producer never waits for the volume: one bounded ring feeds one batch.
 * The ring of the board is small because its DRAM is about a hundred kilobytes
 * and the log must not be the reason a program cannot run. */
#if defined(ESP_PLATFORM)
#define K_LOGFILE_BUFFER_SIZE 480u
#else
#define K_LOGFILE_BUFFER_SIZE 4096u
#endif
#define K_LOGFILE_FLUSH_MS 1000u
#define K_LOGFILE_POWER_WAIT_MS 2000u

static const char g_rotated[] = "log: rotated at 65536 bytes\n";

struct log_image {
    char *text;
    char *previous;
    k_size_t limit;
    k_size_t size;
    k_size_t previous_size;
};

static struct log_image g_boot;
static struct log_image g_run;
static char g_pending[K_LOGFILE_BUFFER_SIZE];
static char g_batch[K_LOGFILE_BUFFER_SIZE];
static k_size_t g_pending_head;
static k_size_t g_pending_size;
static k_size_t g_batch_size;
static k_size_t g_dropped;
static bool g_active;
static bool g_ram;
static bool g_flushing;
static k_u64_t g_last_flush_ms;

static int image_alloc(struct log_image *image, k_size_t limit)
{
    image->limit = limit;
    image->text = kmm_alloc(limit, KMM_PRIORITY_KERNEL);
    image->previous = kmm_alloc(limit, KMM_PRIORITY_KERNEL);

    if (image->text == NULL || image->previous == NULL) {
        kmm_free(image->text);
        kmm_free(image->previous);
        image->text = NULL;
        image->previous = NULL;
        return KERR_NOMEM;
    }

    image->size = 0;
    image->previous_size = 0;
    return 0;
}

static void image_free(struct log_image *image)
{
    kmm_free(image->text);
    kmm_free(image->previous);
    memset(image, 0, sizeof(*image));
}

/* The two equal blocks exchange places, so the old current becomes the one
 * previous generation without copying sixty-four kilobytes. */
static void image_rotate(struct log_image *image)
{
    char *old_text = image->text;

    image->text = image->previous;
    image->previous = old_text;
    image->previous_size = image->size;
    image->size = 0;

    memcpy(image->text, g_rotated, sizeof(g_rotated) - 1u);
    image->size = sizeof(g_rotated) - 1u;
    image->text[image->size] = '\0';
}

static void image_append(struct log_image *image, const char *data, k_size_t len)
{
    while (len > 0) {
        k_size_t room;
        k_size_t chunk;

        if (image->size >= image->limit) {
            image_rotate(image);
        }

        room = image->limit - image->size;
        chunk = K_MIN(room, len);
        memcpy(image->text + image->size, data, chunk);
        image->size += chunk;
        data += chunk;
        len -= chunk;

        /* The file image rotates as soon as a generation reaches 64 KiB; RAM
         * follows the same boundary, including the rotation sentence. */
        if (image->size >= image->limit) {
            image_rotate(image);
        } else {
            image->text[image->size] = '\0';
        }
    }
}

static void pending_clear(void)
{
    g_pending_head = 0;
    g_pending_size = 0;
    g_batch_size = 0;
    g_dropped = 0;
}

/* Keep the newest console bytes when a task outruns the volume. Losing the
 * oldest bytes is explicit in the next batch; losing the caller is not an
 * option, because this function runs on the console's own task. */
static void pending_append(const char *data, k_size_t len)
{
    k_size_t tail;
    k_size_t first;
    k_size_t second;

    if (len > K_LOGFILE_BUFFER_SIZE) {
        g_dropped += len - K_LOGFILE_BUFFER_SIZE;
        data += len - K_LOGFILE_BUFFER_SIZE;
        len = K_LOGFILE_BUFFER_SIZE;
    }

    if (len > K_LOGFILE_BUFFER_SIZE - g_pending_size) {
        k_size_t drop = g_pending_size + len - K_LOGFILE_BUFFER_SIZE;

        g_dropped += drop;
        g_pending_head = (g_pending_head + drop) % K_LOGFILE_BUFFER_SIZE;
        g_pending_size -= drop;
    }

    tail = (g_pending_head + g_pending_size) % K_LOGFILE_BUFFER_SIZE;
    first = K_MIN(len, K_LOGFILE_BUFFER_SIZE - tail);
    second = len - first;
    memcpy(g_pending + tail, data, first);

    if (second > 0) {
        memcpy(g_pending, data + first, second);
    }

    g_pending_size += len;
}

/* Put an unwritten batch back in front of bytes that arrived during the I/O.
 * If the ring is full, the newer bytes are the ones that may be dropped. */
static void pending_prepend(const char *data, k_size_t len)
{
    k_size_t first;
    k_size_t second;

    if (len > K_LOGFILE_BUFFER_SIZE) {
        pending_append(data, len);
        return;
    }

    if (len > K_LOGFILE_BUFFER_SIZE - g_pending_size) {
        k_size_t drop = g_pending_size + len - K_LOGFILE_BUFFER_SIZE;

        g_dropped += drop;
        g_pending_size -= drop;
    }

    g_pending_head = (g_pending_head + K_LOGFILE_BUFFER_SIZE - len) % K_LOGFILE_BUFFER_SIZE;
    first = K_MIN(len, K_LOGFILE_BUFFER_SIZE - g_pending_head);
    second = len - first;
    memcpy(g_pending + g_pending_head, data, first);

    if (second > 0) {
        memcpy(g_pending, data + first, second);
    }

    g_pending_size += len;
}

k_size_t k_logfile_pending(void)
{
    k_size_t size;

    k_port_enter_critical();
    size = g_pending_size;
    k_port_exit_critical();
    return size;
}

bool k_logfile_in_ram(void)
{
    bool ram;

    k_port_enter_critical();
    ram = g_ram;
    k_port_exit_critical();
    return ram;
}

void k_logfile_snapshot(struct k_logfile_snapshot *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    k_port_enter_critical();

    if (g_ram) {
        out->boot = g_boot.text;
        out->boot_size = g_boot.size;
        out->run = g_run.text;
        out->run_size = g_run.size;
        out->run_previous = g_run.previous;
        out->run_previous_size = g_run.previous_size;
    }

    k_port_exit_critical();
}

void k_logfile_console_write(const char *data, k_size_t len)
{
    if (data == NULL || len == 0 || k_console_input_not_logged()) {
        return;
    }

    k_port_enter_critical();

    if (g_active) {
        pending_append(data, len);
    }

    k_port_exit_critical();
}

static int write_all(struct vfs_file *file, const char *data, k_size_t len)
{
    k_size_t done = 0;

    while (done < len) {
        int rc = vfs_write(file, data + done, len - done);

        if (rc <= 0) {
            return rc < 0 ? rc : KERR_IO;
        }
        done += (k_size_t)rc;
    }

    return 0;
}

static int rotate_file(const char *path, const char *previous)
{
    struct vfs_stat info;
    int rc = vfs_stat(path, &info);

    if (rc == KERR_NOENT) {
        return 0;
    }
    if (rc != 0) {
        return rc;
    }

    return vfs_rename(path, previous);
}

/* Append without ever crossing the limit. The generation that reaches 64 KiB
 * is renamed whole, and the new current begins with the sentence that says so. */
static int file_append(const char *path, const char *previous, const char *data, k_size_t len)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_stat info;
    k_u64_t size = 0;
    bool opened = false;
    int rc = vfs_stat(path, &info);

    if (rc == KERR_NOENT) {
        rc = 0;
    } else if (rc == 0) {
        size = info.size;
    } else {
        return rc;
    }

    for (;;) {
        if (size >= K_LOGFILE_LIMIT) {
            if (opened) {
                rc = vfs_close(&file);
                if (rc != 0) {
                    return rc;
                }
                opened = false;
            }

            rc = rotate_file(path, previous);
            if (rc != 0) {
                return rc;
            }

            rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
            if (rc != 0) {
                return rc;
            }
            opened = true;
            rc = write_all(&file, g_rotated, sizeof(g_rotated) - 1u);
            if (rc != 0) {
                (void)vfs_close(&file);
                return rc;
            }
            size = sizeof(g_rotated) - 1u;
        }

        if (!opened) {
            rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &file, priv);
            if (rc != 0) {
                return rc;
            }
            opened = true;
        }

        if (len == 0) {
            break;
        }

        {
            k_size_t room = (k_size_t)(K_LOGFILE_LIMIT - size);
            k_size_t chunk = K_MIN(room, len);

            rc = write_all(&file, data, chunk);
            if (rc != 0) {
                (void)vfs_close(&file);
                return rc;
            }
            size += chunk;
            data += chunk;
            len -= chunk;
        }
    }

    return vfs_close(&file);
}

static int file_replace_boot(const char *data, k_size_t len)
{
    int rc = rotate_file(K_LOGFILE_BOOT_PATH, K_LOGFILE_BOOT_PREVIOUS_PATH);

    if (rc != 0) {
        return rc;
    }

    return file_append(K_LOGFILE_BOOT_PATH, K_LOGFILE_BOOT_PREVIOUS_PATH, data, len);
}

static bool root_is_writable(void)
{
    k_size_t i;

    for (i = 0; i < vfs_mount_count(); i++) {
        const char *name = vfs_mount_name(i);

        if (name != NULL && strcmp(name, "/") == 0) {
            return !vfs_mount_is_read_only(i);
        }
    }

    return false;
}

int k_logfile_start(const char *boot, k_size_t boot_size, bool storage)
{
    bool root_ro = !root_is_writable();
    bool disk = false;
    int rc;

    if (g_active) {
        k_logfile_stop();
    }

    pending_clear();
    g_last_flush_ms = k_sched_uptime_ms();
    g_ram = false;

    if (boot == NULL) {
        boot_size = 0;
    }

    if (storage && !root_ro) {
        rc = vfs_mkdir_p("/etc");
        if (rc == 0) {
            rc = file_replace_boot(boot, boot_size);
        }
        if (rc == 0) {
            rc = file_append(K_LOGFILE_RUN_PATH, K_LOGFILE_RUN_PREVIOUS_PATH, "", 0);
        }
        disk = rc == 0;
    }

    if (!disk) {
        rc = image_alloc(&g_boot, K_LOGFILE_LIMIT_RAM);
        if (rc != 0) {
            return rc;
        }
        rc = image_alloc(&g_run, K_LOGFILE_LIMIT_RAM);
        if (rc != 0) {
            image_free(&g_boot);
            return rc;
        }
        image_append(&g_boot, boot, boot_size);
        g_ram = true;
    }

    g_active = true;

    if (g_ram) {
        char line[128];
        const char *reason = !storage
                                 ? "there is no append volume"
                                 : (root_ro ? "the root is read-only" : "/etc cannot be written");
        int len = snprintf(line, sizeof(line), "log: %s, %s and %s are in RAM\n", reason,
                           K_LOGFILE_BOOT_PATH, K_LOGFILE_RUN_PATH);

        if (len > 0) {
            k_size_t used = (k_size_t)len < sizeof(line) ? (k_size_t)len : sizeof(line) - 1u;

            image_append(&g_boot, line, used);
            k_console_write(line, used);
        }
    } else {
        static const char line[] =
            "log: " K_LOGFILE_BOOT_PATH " and " K_LOGFILE_RUN_PATH " are on the root volume\n";

        k_console_write(line, sizeof(line) - 1u);
    }

    return 0;
}

static int flush_pending(void)
{
    char loss[80];
    k_size_t loss_size = 0;
    k_size_t dropped;
    int rc = 0;

    k_port_enter_critical();
    if (g_flushing) {
        k_port_exit_critical();
        return KERR_BUSY;
    }
    g_flushing = true;
    g_batch_size = g_pending_size;

    if (g_batch_size > 0) {
        k_size_t first = K_MIN(g_batch_size, K_LOGFILE_BUFFER_SIZE - g_pending_head);

        memcpy(g_batch, g_pending + g_pending_head, first);
        if (first < g_batch_size) {
            memcpy(g_batch + first, g_pending, g_batch_size - first);
        }
    }

    g_pending_head = 0;
    g_pending_size = 0;
    dropped = g_dropped;
    g_dropped = 0;
    k_port_exit_critical();

    if (dropped > 0) {
        int len = snprintf(loss, sizeof(loss), "log: %u console byte(s) lost\n", (unsigned)dropped);

        loss_size = len > 0 ? (k_size_t)len : 0;
        if (loss_size >= sizeof(loss)) {
            loss_size = sizeof(loss) - 1u;
        }
    }

    if (loss_size > 0) {
        if (g_ram) {
            k_port_enter_critical();
            image_append(&g_run, loss, loss_size);
            k_port_exit_critical();
        } else {
            rc = file_append(K_LOGFILE_RUN_PATH, K_LOGFILE_RUN_PREVIOUS_PATH, loss, loss_size);
        }
    }

    if (rc == 0 && g_batch_size > 0) {
        if (g_ram) {
            k_port_enter_critical();
            image_append(&g_run, g_batch, g_batch_size);
            k_port_exit_critical();
        } else {
            rc =
                file_append(K_LOGFILE_RUN_PATH, K_LOGFILE_RUN_PREVIOUS_PATH, g_batch, g_batch_size);
        }
    }

    if (rc != 0 && g_batch_size > 0) {
        k_port_enter_critical();
        pending_prepend(g_batch, g_batch_size);
        k_port_exit_critical();
    }

    g_batch_size = 0;
    k_port_enter_critical();
    g_flushing = false;
    k_port_exit_critical();
    return rc;
}

int k_logfile_flush(void)
{
    return flush_pending();
}

int k_logfile_stop_for_poweroff(void)
{
    k_u32_t waited = 0;
    bool was_active;
    int rc;

    k_port_enter_critical();
    was_active = g_active;
    g_active = false;

    /* A writer that already detached a batch owns the VFS until it returns.
     * The power task waits here instead of racing it into an unmount. */
    while (g_flushing && waited < K_LOGFILE_POWER_WAIT_MS) {
        k_port_exit_critical();
        k_sched_sleep_ms(1);
        waited++;
        k_port_enter_critical();
    }

    if (g_flushing) {
        g_active = was_active;
        k_port_exit_critical();
        return KERR_BUSY;
    }

    k_port_exit_critical();

    rc = flush_pending();
    if (rc != 0) {
        /* The clean end is refused before any unmount; the init loop can
         * keep collecting if the machine stays on. */
        k_port_enter_critical();
        g_active = was_active;
        k_port_exit_critical();
        return rc;
    }

    k_port_enter_critical();
    pending_clear();
    k_port_exit_critical();
    return 0;
}

void k_logfile_tick(void)
{
    k_u64_t now = k_sched_uptime_ms();

    if (k_logfile_pending() == 0 || now - g_last_flush_ms < K_LOGFILE_FLUSH_MS) {
        return;
    }

    g_last_flush_ms = now;
    (void)flush_pending();
}

void k_logfile_stop(void)
{
    if (g_active) {
        (void)k_logfile_flush();
    }

    k_port_enter_critical();
    g_active = false;
    g_ram = false;
    pending_clear();
    k_port_exit_critical();

    image_free(&g_boot);
    image_free(&g_run);
}
