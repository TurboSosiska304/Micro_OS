/*
 * Micro OS — the radio of the ESP32: the backend of the WiFi driver.
 *
 * The backend talks to the WiFi hardware of the ESP32 through the driver of
 * ESP-IDF and keeps the configuration of the network in the NVS, so the
 * board joins the same network after every restart.
 *
 * The core of the driver knows nothing about ESP-IDF: it only sees the table
 * of the functions below.
 */
#include "wifi/wifi_esp32.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"
#include "wifi/wifi.h"

/* The namespace of the configuration in the NVS. */
#define NVS_NAMESPACE "microos"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

/* The longest a scan may take before the caller is let go. */
#define WIFI_SCAN_TIMEOUT_MS 10000

static esp_netif_t *s_netif;

/*
 * The driver is made in steps, and a step that fails must not leave a
 * half made driver behind: the state says which of them were made, so
 * that a second attempt picks up where the first one stopped instead of
 * starting the driver of ESP-IDF again (which it cannot: it answers
 * "already initialised" and the events would be watched twice).
 */
static bool s_initialised;  /* the driver of the radio is up */
static bool s_handlers;     /* the events are watched */

static SemaphoreHandle_t s_scan_done;      /* the end of a scan */
static esp_timer_handle_t s_reconnect_timer; /* the wait before a retry */

/* The two below are used by the events and defined after them. */
static bool wifi_reason_is_auth_error(int reason);
static void wifi_schedule_reconnect(k_u32_t delay_ms);

/* Copy a string and always terminate it. */
static void copy_str(char *dst, k_size_t size, const char *src)
{
    if (size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/*
 * The name of the network is 32 bytes long and is not always terminated in
 * the records of the driver.
 */
static void copy_ssid(char *dst, k_size_t size, const uint8_t *src, k_size_t src_size)
{
    k_size_t len = 0;

    if (size == 0) {
        return;
    }

    while (len < src_size && src[len] != '\0') {
        len++;
    }

    if (len > size - 1) {
        len = size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

/*
 * The name of the network in the configuration of the station is a fixed
 * field of 32 bytes: a name of 32 characters has no room for the zero, and
 * the driver of ESP-IDF expects exactly the 32 bytes.
 */
static void copy_sta_ssid(char *dst, k_size_t size, const char *src)
{
    k_size_t len = strlen(src);

    if (len > size) {
        len = size;
    }

    memcpy(dst, src, len);

    if (len < size) {
        dst[len] = '\0';
    }
}

static void copy_bssid(char *dst, k_size_t size, const uint8_t *bssid)
{
    snprintf(dst, size, "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3],
             bssid[4], bssid[5]);
}

/* --- the events of the radio --- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    K_UNUSED(arg);

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            wifi_notify(WIFI_EVT_STARTED, 0);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            wifi_notify(WIFI_EVT_CONNECTED, 0);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *info = (wifi_event_sta_disconnected_t *)data;
            int reason = (info != NULL) ? (int)info->reason : 0;
            k_u32_t delay;

            wifi_notify(WIFI_EVT_DISCONNECTED, reason);

            /*
             * The station does not sit disconnected for ever: a router
             * that was rebooted must not need a hand. The errors that
             * mean "the key does not fit this network" are not asked
             * again - the same answer would come back - and the rest is
             * the policy of the core: how many times and how long to
             * wait. The wait itself is a timer of this port.
             */
            if (!wifi_reason_is_auth_error(reason)) {
                delay = wifi_reconnect_after_ms();
                if (delay > 0) {
                    wifi_schedule_reconnect(delay);
                }
            }

            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            /* The scan is asynchronous: the caller waits on the semaphore. */
            if (s_scan_done != NULL) {
                (void)xSemaphoreGive(s_scan_done);
            }
            break;
        default:
            break;
        }

        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* The address is there: the network is up. */
        wifi_notify(WIFI_EVT_CONNECTED, 0);
    }
}

/*
 * The errors that mean that the key does not fit the network. The access
 * point said no to the key, and asking it the same question again and
 * again would not change the answer. The codes are the ones of the
 * driver of ESP-IDF (wifi_err_reason_t).
 */
static bool wifi_reason_is_auth_error(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return true;
    default:
        return false;
    }
}

static void reconnect_timer_cb(void *arg)
{
    K_UNUSED(arg);

    /* The policy of the core decides once more when the time has come. */
    (void)wifi_reconnect();
}

/*
 * Plan the next attempt of the connection. Only one attempt is planned at
 * a time: a burst of events must not wake the radio up in a pile.
 */
static void wifi_schedule_reconnect(k_u32_t delay_ms)
{
    esp_timer_create_args_t args = {
        .callback = reconnect_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi-reconnect",
        .skip_unhandled_events = true,
    };

    if (s_reconnect_timer == NULL) {
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
            kerror("wifi: the timer of the reconnection cannot be made");
            return;
        }
    }

    (void)esp_timer_stop(s_reconnect_timer);

    if (esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000u) != ESP_OK) {
        kerror("wifi: the reconnection cannot be planned");
    }
}

/* --- the radio itself --- */

static int radio_start(void)
{
    if (esp_wifi_start() != ESP_OK) {
        return -1;
    }

    return 0;
}

static int radio_stop(void)
{
    if (esp_wifi_stop() != ESP_OK) {
        return -1;
    }

    return 0;
}

static int radio_connect(const char *ssid, const char *pass)
{
    wifi_config_t config;
    esp_err_t rc;

    memset(&config, 0, sizeof(config));

    copy_sta_ssid((char *)config.sta.ssid, sizeof(config.sta.ssid), ssid);

    if (pass != NULL && pass[0] != '\0') {
        copy_str((char *)config.sta.password, sizeof(config.sta.password), pass);
    }

    /*
     * The threshold of the authentication is the one of the network that
     * was asked for. A network with a key is joined only by a point that
     * is at least as safe as WPA2: an open point with the same name (the
     * "evil twin") would take the station in and the key it was given
     * would be asked for over the air. A network without a key is joined
     * as it is - it was asked for by name, with nothing to protect.
     */
    config.sta.threshold.authmode =
        (pass != NULL && pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    rc = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (rc != ESP_OK) {
        kerror("wifi: the configuration of the station is refused (rc=%d)", (int)rc);
        return -1;
    }

    rc = esp_wifi_connect();
    if (rc != ESP_OK) {
        kerror("wifi: the connection is refused (rc=%d)", (int)rc);
        return -1;
    }

    return 0;
}

static int radio_disconnect(void)
{
    if (esp_wifi_disconnect() != ESP_OK) {
        return -1;
    }

    return 0;
}

static int radio_scan(struct wifi_scan_entry *out, k_size_t max, k_size_t *count)
{
    wifi_scan_config_t config;
    wifi_ap_record_t *records;
    uint16_t number;
    uint16_t i;
    esp_err_t rc;

    if (count != NULL) {
        *count = 0;
    }

    memset(&config, 0, sizeof(config));
    config.show_hidden = false;

    /*
     * The scan is started without blocking the caller and waited for with
     * a deadline: a radio that went quiet must not hold the shell of the
     * board for ever. A scan that was left behind by an earlier caller
     * must not answer this one, so the semaphore is emptied first.
     */
    while (xSemaphoreTake(s_scan_done, 0) == pdTRUE) {
    }

    rc = esp_wifi_scan_start(&config, false);
    if (rc != ESP_OK) {
        kerror("wifi: the scan does not start (rc=%d)", (int)rc);
        return -1;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS)) != pdTRUE) {
        (void)esp_wifi_scan_stop();
        kerror("wifi: the scan did not answer in %u ms", (unsigned)WIFI_SCAN_TIMEOUT_MS);
        return -1;
    }

    rc = esp_wifi_scan_get_ap_num(&number);
    if (rc != ESP_OK) {
        return -1;
    }

    if (number == 0) {
        return 0;
    }

    if ((k_size_t)number > max) {
        number = (uint16_t)max;
    }

    records = heap_caps_malloc((size_t)number * sizeof(wifi_ap_record_t), MALLOC_CAP_INTERNAL);
    if (records == NULL) {
        kerror("wifi: no memory for the records of the scan");
        return -1;
    }

    rc = esp_wifi_scan_get_ap_records(&number, records);
    if (rc != ESP_OK) {
        heap_caps_free(records);
        return -1;
    }

    for (i = 0; i < number; i++) {
        copy_ssid(out[i].ssid, sizeof(out[i].ssid), records[i].ssid, sizeof(records[i].ssid));
        copy_bssid(out[i].bssid, sizeof(out[i].bssid), records[i].bssid);
        out[i].rssi = records[i].rssi;
        out[i].channel = records[i].primary;
        out[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }

    heap_caps_free(records);

    if (count != NULL) {
        *count = number;
    }

    return 0;
}

static int radio_status(struct wifi_status *out)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;

    if (out == NULL) {
        return -1;
    }

    memset(&ap, 0, sizeof(ap));

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        copy_ssid(out->ssid, sizeof(out->ssid), ap.ssid, sizeof(ap.ssid));
        copy_bssid(out->bssid, sizeof(out->bssid), ap.bssid);
        out->rssi = ap.rssi;
        out->channel = ap.primary;
    }

    memset(&ip, 0, sizeof(ip));

    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }

    return 0;
}

/* --- the storage of the configuration --- */

static int storage_load(char *ssid, k_size_t ssid_size, char *pass, k_size_t pass_size)
{
    nvs_handle_t handle;
    size_t len;
    esp_err_t rc;

    if (ssid == NULL || ssid_size == 0 || pass == NULL || pass_size == 0) {
        return -1;
    }

    rc = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (rc != ESP_OK) {
        return -1;
    }

    len = ssid_size;
    rc = nvs_get_str(handle, NVS_KEY_SSID, ssid, &len);
    if (rc != ESP_OK) {
        nvs_close(handle);
        return -1;
    }
    ssid[ssid_size - 1] = '\0';

    /* The key of the network is optional: an open network has none. */
    len = pass_size;
    rc = nvs_get_str(handle, NVS_KEY_PASS, pass, &len);
    if (rc != ESP_OK) {
        pass[0] = '\0';
    } else {
        pass[pass_size - 1] = '\0';
    }

    nvs_close(handle);

    return 0;
}

static int storage_save(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t rc;

    rc = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        kerror("wifi: the NVS cannot be opened (rc=%d)", (int)rc);
        return -1;
    }

    rc = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (rc == ESP_OK) {
        rc = nvs_set_str(handle, NVS_KEY_PASS, pass != NULL ? pass : "");
    }

    if (rc == ESP_OK) {
        rc = nvs_commit(handle);
    }

    nvs_close(handle);

    return (rc == ESP_OK) ? 0 : -1;
}

static int storage_erase(void)
{
    nvs_handle_t handle;
    esp_err_t rc;
    esp_err_t erase_rc;

    rc = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        return -1;
    }

    erase_rc = nvs_erase_key(handle, NVS_KEY_SSID);
    if (erase_rc != ESP_OK && erase_rc != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return -1;
    }

    erase_rc = nvs_erase_key(handle, NVS_KEY_PASS);
    if (erase_rc != ESP_OK && erase_rc != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return -1;
    }

    rc = nvs_commit(handle);
    nvs_close(handle);

    return (rc == ESP_OK) ? 0 : -1;
}

/* --- the tables of the core --- */

static const struct wifi_radio s_radio_ops = {
    .start = radio_start,
    .stop = radio_stop,
    .connect = radio_connect,
    .disconnect = radio_disconnect,
    .scan = radio_scan,
    .status = radio_status,
};

static const struct wifi_storage s_storage_ops = {
    .load = storage_load,
    .save = storage_save,
    .erase = storage_erase,
};

/* --- the installation of the backend --- */

/*
 * The NVS holds the calibration data of the radio as well. A partition with
 * an old format is erased and initialised again.
 */
static int nvs_init(void)
{
    esp_err_t rc = nvs_flash_init();

    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        kwarn("wifi: the NVS is erased and initialised again");

        rc = nvs_flash_erase();
        if (rc != ESP_OK) {
            kerror("wifi: the NVS cannot be erased (rc=%d)", (int)rc);
            return -1;
        }

        rc = nvs_flash_init();
    }

    if (rc != ESP_OK) {
        kerror("wifi: the NVS cannot be initialised (rc=%d)", (int)rc);
        return -1;
    }

    return 0;
}

int wifi_esp32_init(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t rc;

    if (s_initialised && s_handlers) {
        return 0;
    }

    if (nvs_init() != 0) {
        return -1;
    }

    if (s_scan_done == NULL) {
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_done == NULL) {
            kerror("wifi: the semaphore of the scan cannot be made");
            return -1;
        }
    }

    rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        kerror("wifi: the netif cannot be initialised (rc=%d)", (int)rc);
        return -2;
    }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        kerror("wifi: the event loop cannot be created (rc=%d)", (int)rc);
        return -3;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        /* The station is already there: somebody else has created it. */
        s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }

    /*
     * The driver of the radio is the point of no return: from here on
     * ESP-IDF owns the state of the chip. "Already initialised" is not a
     * failure - it is the answer of a second attempt after a step that
     * failed further down - and the driver says that it is up.
     */
    rc = esp_wifi_init(&config);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        kerror("wifi: the driver of the radio does not start (rc=%d)", (int)rc);
        return -4;
    }
    s_initialised = true;

    /*
     * The events are watched once. A second attempt that registered the
     * handlers again would make every event of the radio be reported to
     * the core twice.
     */
    if (!s_handlers) {
        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                NULL, NULL) != ESP_OK) {
            kerror("wifi: the events of the radio cannot be watched");
            return -5;
        }

        if (esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                NULL, NULL) != ESP_OK) {
            kerror("wifi: the events of the network cannot be watched");
            return -6;
        }

        s_handlers = true;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        kerror("wifi: the station cannot be selected");
        return -7;
    }

    /*
     * The configuration of a network is kept by this driver, in its own
     * records of the NVS, and the driver of ESP-IDF keeps its own copy
     * only in the working memory. With its storage in the flash every
     * connection would be a write to the flash of the board, and the
     * flash has a limited number of those.
     */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_set_radio(&s_radio_ops);
    wifi_set_storage(&s_storage_ops);

    return 0;
}

int wifi_esp32_get_mac(uint8_t *mac)
{
    if (mac == NULL) {
        return -1;
    }

    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return -1;
    }

    return 0;
}
