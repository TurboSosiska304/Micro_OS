/*
 * Micro OS — the WiFi station: the portable core of the driver.
 *
 * The driver is split in two parts:
 *
 *   - the core (components/wifi/src/wifi.c, portable C11): the state machine
 *     of the station, the configuration and the formatting of the status. It
 *     knows nothing about the hardware;
 *   - the radio (the backend): the ESP-IDF driver of the ESP32, which talks
 *     to the hardware and reports the events back to the core.
 *
 * The radio is a table of functions that the backend fills in, so the whole
 * logic of the driver can be tested on the host, without a radio.
 */
#ifndef MICRO_OS_WIFI_WIFI_H
#define MICRO_OS_WIFI_WIFI_H

#include "kernel/types.h"

/* The limits of the IEEE 802.11: 32 characters of the name, 63 of the key. */
#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65
#define WIFI_BSSID_MAX 18
#define WIFI_IP_MAX 16

/* The number of the networks one scan reports. */
#define WIFI_SCAN_MAX 16

/* The result codes of the driver. */
#define WIFI_OK 0
#define WIFI_ERROR (-1)
#define WIFI_NO_RADIO (-2)    /* there is no radio at all */
#define WIFI_NOT_STARTED (-3) /* the radio is off */
#define WIFI_INVALID (-4)     /* the arguments are wrong */
#define WIFI_NO_CONFIG (-5)   /* there is no saved configuration */
#define WIFI_NO_STORAGE (-6)  /* there is no storage for the configuration */

/* The states of the station. */
typedef enum {
    WIFI_STATE_STOPPED = 0,  /* the radio is off */
    WIFI_STATE_IDLE,         /* the radio is on, there is no network */
    WIFI_STATE_CONNECTING,   /* the association is in progress */
    WIFI_STATE_CONNECTED,    /* the network is up */
    WIFI_STATE_DISCONNECTED, /* the network is gone */
    WIFI_STATE_FAILED,       /* the last attempt failed */
} wifi_state_t;

/* The events the radio reports to the core. */
#define WIFI_EVT_STARTED 1
#define WIFI_EVT_CONNECTED 2
#define WIFI_EVT_DISCONNECTED 3
#define WIFI_EVT_FAILED 4

/* The information about the network the station is attached to. */
struct wifi_status {
    wifi_state_t state; /* the state belongs to the core, not to the radio */
    char ssid[WIFI_SSID_MAX];
    char bssid[WIFI_BSSID_MAX];
    char ip[WIFI_IP_MAX];
    int rssi; /* dBm */
    int channel;
    int reason; /* the code of the last failure */
};

/* One network of a scan. */
struct wifi_scan_entry {
    char ssid[WIFI_SSID_MAX];
    char bssid[WIFI_BSSID_MAX];
    int rssi;
    int channel;
    bool secure;
};

/*
 * The radio: the backend implements it. Every function returns 0 on success
 * and a negative code on failure. The radio fills the fields of the network
 * (the name, the address, the signal), the state belongs to the core.
 */
struct wifi_radio {
    int (*start)(void);
    int (*stop)(void);
    int (*connect)(const char *ssid, const char *pass);
    int (*disconnect)(void);
    int (*scan)(struct wifi_scan_entry *out, k_size_t max, k_size_t *count);
    int (*status)(struct wifi_status *out);
};

/*
 * The storage of the configuration: the backend implements it. On the ESP32
 * it is the NVS, so the network survives the restart of the board.
 */
struct wifi_storage {
    int (*load)(char *ssid, k_size_t ssid_size, char *pass, k_size_t pass_size);
    int (*save)(const char *ssid, const char *pass);
    int (*erase)(void);
};

/* Install the radio and the storage. The backend does it at the boot. */
void wifi_set_radio(const struct wifi_radio *radio);
void wifi_set_storage(const struct wifi_storage *storage);

/* The core: the state machine of the station. */
int wifi_init(void);
int wifi_start(void);
int wifi_stop(void);
int wifi_connect(const char *ssid, const char *pass);
int wifi_disconnect(void);
int wifi_scan(struct wifi_scan_entry *out, k_size_t max, k_size_t *count);
int wifi_get_status(struct wifi_status *out);

/* The configuration of the network: the name and the key. */
int wifi_config_save(const char *ssid, const char *pass);
int wifi_config_load(void);
int wifi_config_forget(void);
int wifi_config_get(char *ssid, k_size_t ssid_size, char *pass, k_size_t pass_size);

/* Connect with the saved configuration. */
int wifi_connect_saved(void);

/* The event of the radio: the core changes its state. */
void wifi_notify(int event, int reason);

/* The name of a state, for the shell and the log. */
const char *wifi_state_name(wifi_state_t state);

/*
 * The formatting of the status and of one network of a scan. Both write
 * into the buffer of the caller and return the number of the characters
 * that would be needed (like snprintf does).
 */
int wifi_format_status(char *buf, k_size_t size, const struct wifi_status *status);
int wifi_format_scan_entry(char *buf, k_size_t size, const struct wifi_scan_entry *entry);

#endif /* MICRO_OS_WIFI_WIFI_H */
