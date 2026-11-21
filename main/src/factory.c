/*
 * Micro OS — the factory reset of the board.
 *
 * Two stores are erased: the volume of MicroFS (the files of the user and
 * the boot log) and the NVS (the files of /etc, the saved network and the
 * calibration of the radio). The NVS is erased as a whole instead of one
 * namespace at a time, because "factory" means that nothing of the last
 * life is left — and the port knows how to bring a store of a fresh
 * partition up, so the next boot finds an empty one and creates it.
 *
 * A reset does not leave /etc empty: the board of a person who has never
 * used it has the setting of its own, and the boot writes it whenever the
 * file is missing, so a formatted volume, a lost store and a reset all end in
 * the same tree. The alarm of the clock is not touched at all: it lives in
 * the BM8563 and is the one way an off board comes back, so a reset that
 * wiped it would leave a board on a battery that nobody can wake.
 *
 * The order matters: the volume is unmounted before it is formatted, and
 * the reboot happens after both erases, so a failure leaves a board that
 * still has a console to say what went wrong. The command that formats the
 * volume on its own (`mkfs -y`) is the way back from a half done reset.
 */
#include "factory.h"

#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#include "nvs_flash.h"

/* The last line of the console must leave the UART before the chip resets. */
#define FACTORY_QUIET_MS 300

/* The directory of the configuration and the scripts of the init process. */
#define FACTORY_ETC_DIR "/etc"
#define FACTORY_INITD_DIR "/etc/init.d"

/*
 * The files of /etc that a board of this model has before anybody writes to
 * it: one setting, in the shape the other ports use (`hello`, `hostname`,
 * `console`). The store keeps them like any other file after the boot, so
 * after a reset they come from here and not from the NVS.
 */
static const struct factory_file {
    const char *path;
    const char *text;
} g_factory_files[] = {
    {FACTORY_ETC_DIR "/setting", "hello\nhostname=stickc-plus2\nconsole=tft\n"},
};

/* One file of the defaults, written only when it is not there: a file of the
 * person is never replaced by the firmware. */
static int write_default(const struct factory_file *file)
{
    struct vfs_stat info;
    struct vfs_file handle;
    /* The private area of the open file: 64 bytes, on the stack of the task
     * that writes the default. */
    k_u8_t data[VFS_FILE_DATA_SIZE];
    k_size_t size = strlen(file->text);
    int rc;

    rc = vfs_stat(file->path, &info);
    if (rc == 0) {
        return 0;
    }
    if (rc != KERR_NOENT) {
        kwarn("factory: cannot look at %s (rc=%d)", file->path, rc);
        return rc;
    }

    rc = vfs_open(file->path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &handle, data);
    if (rc != 0) {
        kwarn("factory: cannot write %s (rc=%d)", file->path, rc);
        return rc;
    }

    rc = vfs_write(&handle, file->text, size);
    if (rc < 0) {
        (void)vfs_close(&handle);
        kwarn("factory: cannot write %s (rc=%d)", file->path, rc);
        return rc;
    }

    (void)vfs_close(&handle);
    (void)vfs_chmod(file->path, 0644u);

    return 1;
}

int factory_defaults(void)
{
    int written = 0;
    k_size_t i;
    int rc;

    rc = vfs_mkdir_p(FACTORY_ETC_DIR);
    if (rc != 0) {
        kwarn("factory: cannot create %s (rc=%d)", FACTORY_ETC_DIR, rc);
        return rc;
    }

    /* The scripts of the init process have their directory even when no
     * script asks for it: it is where a person puts one. */
    rc = vfs_mkdir_p(FACTORY_INITD_DIR);
    if (rc != 0) {
        kwarn("factory: cannot create %s (rc=%d)", FACTORY_INITD_DIR, rc);
    }

    for (i = 0; i < K_ARRAY_SIZE(g_factory_files); i++) {
        rc = write_default(&g_factory_files[i]);
        if (rc > 0) {
            written++;
        }
    }

    return written;
}

int factory_reset(void)
{
    esp_err_t rc;

    kinfo("factory: erasing the volume and the store");

    /* The volume cannot be formatted while it is mounted. */
    mfs_unmount();
    vfs_umount("/");

    if (mfs_vfs_format("flash0") != 0) {
        kerror("factory: the volume cannot be erased");
        return -1;
    }

    /*
     * The whole NVS partition: the configuration of /etc, the network of
     * the last time, the calibration of the radio. The next boot remakes
     * all of it; the radio calibrates itself again, which costs a moment
     * of the boot and nothing else.
     */
    rc = nvs_flash_erase();
    if (rc != ESP_OK) {
        kerror("factory: the store cannot be erased (rc=%d)", (int)rc);
        return -2;
    }

    /*
     * The next boot would write the defaults anyway, but the volume of a
     * board that never reaches the boot would stay empty and a person who
     * reads the flash would think the reset erased more than it did.
     */
    if (mfs_vfs_mount("/", "flash0") == 0) {
        int files = factory_defaults();

        if (files >= 0) {
            kinfo("factory: %d default file(s) of /etc written", files);
        }
    }

    return 0;
}

static int cmd_factory(int argc, char **argv)
{
    int rc;

    if (argc != 3 || strcmp(argv[1], "reset") != 0 || strcmp(argv[2], "-y") != 0) {
        if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
            /*
             * The flag is missing: the line is a question, not an order,
             * and the answer is a sentence that says what would be lost.
             */
            kshell_printf("factory reset: every file and every setting will be erased\n");
            kshell_printf("factory reset: add -y to do it: factory reset -y\n");
        } else {
            kshell_printf("usage: factory reset -y   (erases the volume and the store)\n");
        }

        return KSHELL_USAGE;
    }

    kshell_printf("factory reset: erasing, the board restarts by itself\n");

    rc = factory_reset();
    if (rc != 0) {
        kshell_printf("factory reset: the reset is not complete (rc=%d)\n", rc);
        kshell_printf("factory reset: `mkfs -y` formats the volume again\n");
        return KSHELL_ERROR;
    }

    /* One wait: the line that says the reset is done has to leave the UART
     * before the chip resets, or the console of the next boot is all that is
     * left of it. */
    kshell_printf("factory reset: done, restarting\n");

    vTaskDelay(pdMS_TO_TICKS(FACTORY_QUIET_MS));

    esp_restart();
    /* esp_restart() is declared noreturn; the line keeps the type of the
     * function honest when the attribute is not seen. */
    return KSHELL_OK;
}

int factory_register(void)
{
    return kshell_register("factory", "the factory reset: factory reset -y", cmd_factory);
}
