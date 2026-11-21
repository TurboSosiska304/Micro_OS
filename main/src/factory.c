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
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#include "nvs_flash.h"

/* The last line of the console must leave the UART before the chip resets. */
#define FACTORY_QUIET_MS 300

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
