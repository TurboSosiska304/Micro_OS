/*
 * Micro OS — WiFi driver of the StickC Plus2.
 *
 * The check starts the station mode and reads the MAC address. Without a
 * configured access point the module is "degraded": the radio works, but
 * there is no connection yet.
 */
#include "drivers/internal.h"

#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "kernel/kmod.h"
#include "kernel/log.h"

static bool s_started;
static bool s_connected;
static uint8_t s_mac[6];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_connected = true;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
    }
}

static int drv_wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    /* The WiFi driver stores its calibration data in the NVS. */
    {
        esp_err_t rc = nvs_flash_init();

        if (rc != ESP_OK && rc != ESP_ERR_NVS_NO_FREE_PAGES &&
            rc != ESP_ERR_NVS_NEW_VERSION_FOUND) {
            kerror("wifi: cannot initialise the NVS (rc=%d)", (int)rc);
            return -1;
        }
    }

    if (esp_netif_init() != ESP_OK) {
        return -2;
    }

    if (esp_event_loop_create_default() != ESP_OK) {
        return -3;
    }

    if (esp_wifi_init(&cfg) != ESP_OK) {
        kerror("wifi: esp_wifi_init failed");
        return -4;
    }

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
                                            NULL) != ESP_OK) {
        return -4;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        return -5;
    }

    if (esp_wifi_start() != ESP_OK) {
        return -6;
    }

    if (esp_wifi_get_mac(WIFI_IF_STA, s_mac) != ESP_OK) {
        return -7;
    }

    s_started = true;
    return 0;
}

static int drv_wifi_check(void)
{
    if (!s_started) {
        return -1;
    }

    /* The radio is up: without an access point the module is only degraded. */
    return s_connected ? 0 : 1;
}

static const char *drv_wifi_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x%s", s_mac[0], s_mac[1], s_mac[2],
             s_mac[3], s_mac[4], s_mac[5], s_connected ? " connected" : " idle");
    return buf;
}

static const struct kmod s_wifi_module = {
    .name = "wifi",
    .desc = "WiFi radio (ESP32-PICO-V3-02)",
    .init = drv_wifi_init,
    .check = drv_wifi_check,
    .detail = drv_wifi_detail,
};

int drv_wifi_register(void)
{
    return kmod_register(&s_wifi_module);
}
