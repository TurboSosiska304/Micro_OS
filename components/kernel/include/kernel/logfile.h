/*
 * Micro OS — the logs of the machine.
 *
 * The boot capture is installed once, after the root exists. The console only
 * copies its bytes into a bounded memory buffer here; a separate task writes
 * batches through the VFS, so a slow volume never stands between a person and
 * the prompt. A read-only root, or a port without an append volume, keeps the
 * same two generations in RAM and says so on the console.
 */
#ifndef MICRO_OS_KERNEL_LOGFILE_H
#define MICRO_OS_KERNEL_LOGFILE_H

#include "kernel/types.h"

#define K_LOGFILE_BOOT_PATH "/etc/bootlog.log"
#define K_LOGFILE_BOOT_PREVIOUS_PATH "/etc/bootlog.1.log"
#define K_LOGFILE_RUN_PATH "/etc/run.log"
#define K_LOGFILE_RUN_PREVIOUS_PATH "/etc/run.1.log"
#define K_LOGFILE_LIMIT (64u * 1024u)

/* The log of a machine without a volume lives in its RAM: the boundary of a
 * file stays 64 KiB, the RAM copy keeps the recent lines and says so. */
#if defined(ESP_PLATFORM)
#define K_LOGFILE_LIMIT_RAM (2u * 1024u)
#else
#define K_LOGFILE_LIMIT_RAM (16u * 1024u)
#endif

/* The bytes waiting for the next batch, and whether this boot has no volume. */
k_size_t k_logfile_pending(void);
bool k_logfile_in_ram(void);

/* A read-only snapshot of the RAM images. The pointers live until the next
 * flush, start or stop; a disk-backed log leaves them NULL. */
struct k_logfile_snapshot {
    const char *boot;
    k_size_t boot_size;
    const char *run;
    k_size_t run_size;
    const char *run_previous;
    k_size_t run_previous_size;
};

void k_logfile_snapshot(struct k_logfile_snapshot *out);

/*
 * Install the captured boot and begin the console log. `storage` is false for
 * a port whose /etc has no volume it may append to (the board); true asks the
 * root mount and falls back to RAM when that root is read-only or refuses.
 * Returns 0 for either a disk or a RAM log, or a negative kernel error.
 */
int k_logfile_start(const char *boot, k_size_t boot_size, bool storage);

/* The console hook: copy only, never open or write a file. */
void k_logfile_console_write(const char *data, k_size_t len);

/* Write one detached batch. Safe to call from a test without the service. */
int k_logfile_flush(void);

/*
 * The last step of a clean poweroff: stop accepting console bytes, wait for a
 * batch already being written, flush the remainder and clear the ring. The
 * caller does this before it syncs and unmounts the volumes. Returns 0, or a
 * kernel error if the writer did not finish or the last batch did not reach
 * the volume; on an error the log accepts bytes again and the machine stays on.
 */
int k_logfile_stop_for_poweroff(void);

/* Called by the existing init loop; flush at most once a second. */
void k_logfile_tick(void);

/* Flush and forget the active log. A later start may install a new boot. */
void k_logfile_stop(void);

#endif /* MICRO_OS_KERNEL_LOGFILE_H */
