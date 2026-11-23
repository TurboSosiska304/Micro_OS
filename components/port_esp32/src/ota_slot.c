/*
 * Micro OS — the slots of the board: ota_0 and ota_1 (net/ota.h, ADR 0022).
 *
 * The bootloader of the ESP32 keeps the state of the two application slots
 * in otadata, and with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE it asks a new
 * image to confirm itself: a slot that an update selected boots as NEW, the
 * first boot turns it PENDING_VERIFY, and a slot that never confirms itself
 * is ABORTED on the next reset while the other one starts. That state
 * machine is the one net/ota.h describes, and it already lives in the
 * silicon and in the ROM of the chip: this file is the half of it that a C
 * program can see.
 *
 * The image of the board is an ESP-IDF application, and its signature
 * trailer is not after the payload on the flash: it is in the last 4 KiB of
 * the slot (ADR 0020). The portable half of the update checks the trailer
 * before it writes anything; this file writes the payload through the API of
 * app_update and puts the trailer where the boot reads it.
 */
#include "port_esp32.h"

#include <string.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "kernel/log.h"
#include "net/ota.h"

/* The last 4 KiB of a slot are the trailer of the image (ADR 0020). */
#define OTA_SLOT_TRAILER_RESERVE 0x1000u

static const esp_partition_t *s_partition;
static esp_ota_handle_t s_handle;
static k_u64_t s_offset;
static bool s_open;

static const esp_partition_t *slot_partition(int slot)
{
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
}

static int slot_of(const esp_partition_t *partition)
{
    if (partition == NULL) {
        return -1;
    }

    if (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return 0;
    }

    if (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return 1;
    }

    return -1;
}

int ota_slot_begin(int slot)
{
    if (slot != 0 && slot != 1) {
        return OTA_EIO;
    }

    s_partition = slot_partition(slot);
    if (s_partition == NULL) {
        kwarn("ota: the board has no slot ota_%d", slot);
        return OTA_EIO;
    }

    if (esp_ota_begin(s_partition, OTA_SIZE_UNKNOWN, &s_handle) != ESP_OK) {
        kwarn("ota: ota_%d did not open for writing", slot);
        return OTA_EIO;
    }

    s_offset = 0;
    s_open = true;

    return OTA_OK;
}

int ota_slot_write(int slot, k_u64_t offset, const void *data, k_size_t size)
{
    (void)slot;

    if (!s_open || offset != s_offset) {
        return OTA_EIO;
    }

    if (esp_ota_write(s_handle, data, size) != ESP_OK) {
        return OTA_EIO;
    }

    s_offset += size;

    return OTA_OK;
}

int ota_slot_end(int slot, const k_u8_t trailer[OTA_TRAILER_SIZE])
{
    k_u64_t trailer_at;

    if (!s_open) {
        return OTA_EIO;
    }

    s_open = false;

    /* The image of the application is whole: its header, its segments and
     * the checksum of the format. A slot that fails here is not selected. */
    if (esp_ota_end(s_handle) != ESP_OK) {
        kwarn("ota: the image of ota_%d is not one of an application", slot);
        return OTA_EIO;
    }

    if (s_partition->size < OTA_SLOT_TRAILER_RESERVE) {
        return OTA_EIO;
    }

    trailer_at = s_partition->size - OTA_SLOT_TRAILER_RESERVE;

    if (esp_partition_erase_range(s_partition, trailer_at, OTA_SLOT_TRAILER_RESERVE) != ESP_OK) {
        return OTA_EIO;
    }

    if (esp_partition_write(s_partition, trailer_at, trailer, OTA_TRAILER_SIZE) != ESP_OK) {
        return OTA_EIO;
    }

    return OTA_OK;
}

int ota_slot_select(int slot)
{
    const esp_partition_t *partition = slot_partition(slot);

    if (partition == NULL) {
        return OTA_EIO;
    }

    /* The bootloader writes the state NEW into otadata: the next reset runs
     * this slot, and its first boot has to confirm itself. */
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        return OTA_EIO;
    }

    return OTA_OK;
}

int ota_slot_invalidate(int slot)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (slot_of(running) != slot) {
        /* The other slot is not the one this boot runs: nothing selects it
         * any more, and the next install overwrites it. */
        return OTA_OK;
    }

    /* The running slot is the one that must not be kept. The bootloader has
     * the other image and starts it; this call does not return when that is
     * possible. */
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();

    return OTA_EIO;
}

int ota_slot_size(int slot, k_u64_t *bytes)
{
    const esp_partition_t *partition = slot_partition(slot);

    if (partition == NULL || bytes == NULL) {
        return OTA_EIO;
    }

    *bytes = partition->size;

    return OTA_OK;
}

/* --- the state of the boot --- */

static int state_of(esp_ota_img_states_t image_state)
{
    switch (image_state) {
    case ESP_OTA_IMG_NEW:
        return OTA_SLOT_NEW;
    case ESP_OTA_IMG_PENDING_VERIFY:
        return OTA_SLOT_PENDING;
    case ESP_OTA_IMG_INVALID:
    case ESP_OTA_IMG_ABORTED:
        return OTA_SLOT_INVALID;
    default:
        return OTA_SLOT_VALID;
    }
}

int ota_status(struct ota_state *out)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    int slot = slot_of(running);

    if (out == NULL || slot < 0) {
        return OTA_EIO;
    }

    memset(out, 0, sizeof(*out));
    out->active = slot;

    if (esp_ota_get_state_partition(running, &image_state) == ESP_OK) {
        out->slot_state = state_of(image_state);
    } else {
        out->slot_state = OTA_SLOT_VALID;
    }

    return OTA_OK;
}

/*
 * The boot of the board: the bootloader has already decided - a PENDING
 * slot it started is the one this code runs in, and a slot that never
 * confirmed itself was rolled back before the kernel existed. There is
 * nothing for this call to write; it reads the state and says it.
 */
int ota_boot(struct ota_state *out)
{
    return ota_status(out);
}

int ota_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;

    if (running == NULL) {
        return OTA_EIO;
    }

    /* A slot that is already valid stays valid: the call of init runs at
     * every boot, and only a PENDING_VERIFY slot has something to confirm. */
    if (esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state != ESP_OTA_IMG_PENDING_VERIFY && image_state != ESP_OTA_IMG_NEW) {
        return OTA_OK;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        kwarn("ota: the slot of this boot did not confirm itself");
        return OTA_EIO;
    }

    kinfo("ota: slot %d is valid, the rollback is cancelled", slot_of(running));

    return OTA_OK;
}
