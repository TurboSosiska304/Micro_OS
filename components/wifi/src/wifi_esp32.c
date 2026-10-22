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
#include "esp_wifi.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"

/* The namespace of the configuration in the NVS. */
#define NVS_NAMESPACE "microos"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

static esp_netif_t *s_netif;
static bool s_initialised;

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

            wifi_notify(WIFI_EVT_DISCONNECTED, reason);
            break;
        }
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
     * The threshold of the authentication stays open: the access point
     * decides what it accepts, and a wrong key is reported as a failure of
     * the connection instead of a silent refusal.
     */
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

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

    /* The scan blocks: the caller waits for the result. */
    rc = esp_wifi_scan_start(&config, true);
    if (rc != ESP_OK) {
        kerror("wifi: the scan does not start (rc=%d)", (int)rc);
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

    if (s_initialised) {
        return 0;
    }

    if (nvs_init() != 0) {
        return -1;
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

    if (esp_wifi_init(&config) != ESP_OK) {
        kerror("wifi: the driver of the radio does not start");
        return -4;
    }

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
                                            NULL) != ESP_OK) {
        kerror("wifi: the events of the radio cannot be watched");
        return -5;
    }

    if (esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
                                            NULL) != ESP_OK) {
        kerror("wifi: the events of the network cannot be watched");
        return -6;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        kerror("wifi: the station cannot be selected");
        return -7;
    }

    wifi_set_radio(&s_radio_ops);
    wifi_set_storage(&s_storage_ops);

    s_initialised = true;

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
