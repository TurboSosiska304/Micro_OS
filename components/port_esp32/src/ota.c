/*
 * Micro OS — the confirmation of a boot and the rollback of the board
 * (ADR 0022).
 *
 * The bootloader of the ESP32 has two application slots and the otadata
 * partition that selects one. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the
 * first boot of a slot that an update wrote is PENDING_VERIFY: the
 * bootloader asks the application to confirm itself, and a boot that ends
 * before the confirmation (a watchdog, a panic, a halt) is marked ABORTED on
 * the next reset and the previous slot is started.
 *
 * This file is the half of the application: it confirms the boot once the
 * system is up (the self test, the doctor and the shell), and it is the way
 * a boot that must not be kept - a wrong signature with the policy on -
 * gives the board back to the other slot instead of stopping it.
 *
 * The confirmation is a write of four bytes in the otadata sector, not an
 * eFuse: a person who flashes the board can still change the slots, and
 * ADR 0020 says that this is the promise the tree makes.
 */
#include "port_esp32.h"

#include "esp_ota_ops.h"
#include "kernel/log.h"

/*
 * The update of ADR 0022 is the state machine of the bootloader: a slot that
 * an update wrote is PENDING_VERIFY until the shell confirms it, and a boot
 * that ends before that line is ABORTED on the next reset. Without the option
 * the bootloader marks the new image valid before it runs, and the rollback
 * this file promises does not exist. A stale sdkconfig is caught here, at
 * build time, rather than on a board that cannot return to its old slot.
 */
#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Micro OS: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is required (ADR 0022)"
#endif

/* The state of a slot, for the console. */
static const char *state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    default:
        return "undefined";
    }
}

/*
 * The boot is up: keep the slot. Called by the task of the shell before its
 * first line, so the confirmation is the proof that the kernel, the
 * filesystem, the doctor, the self test and the console of the board work.
 */
int port_esp32_ota_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t rc;

    if (running == NULL) {
        kwarn("boot: the running partition is not known, the slot is not confirmed");
        return -1;
    }

    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        kinfo("boot: running from %s (%s)", running->label, state_name(state));
    } else {
        kinfo("boot: running from %s", running->label);
    }

    rc = esp_ota_mark_app_valid_cancel_rollback();
    if (rc != ESP_OK) {
        kerror("boot: the image of %s is not confirmed (0x%x)", running->label, (unsigned)rc);
        return -1;
    }

    kinfo("boot: the image of %s is valid, the rollback is cancelled", running->label);

    return 0;
}

/*
 * Give the board back to the other slot. A boot that must not be kept calls
 * this instead of halting: the slot is marked INVALID, the bootloader reads
 * the other otadata sector and the previous image starts. With nothing to
 * roll back to the call answers an error, and the caller halts.
 */
void port_esp32_ota_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t rc;

    if (running != NULL) {
        kerror("boot: %s is not kept, the board returns to the other slot", running->label);
    }

    rc = esp_ota_mark_app_invalid_rollback_and_reboot();

    /* The call restarts the board when the rollback is possible. */
    kerror("boot: the rollback failed (0x%x): there is no other image to run", (unsigned)rc);
}
