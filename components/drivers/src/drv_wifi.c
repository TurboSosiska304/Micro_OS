/*
 * Micro OS — WiFi driver of the StickC Plus2.
 *
 * The driver is split in two: the core of the station (components/wifi, the
 * state machine and the configuration) and the radio of the ESP32 (the
 * backend, which talks to the hardware). This file is the glue: it starts
 * the radio, keeps the module of the hardware doctor and gives the shell
 * the command that talks to the network.
 *
 * Without a configured access point the module is "degraded": the radio
 * works, but there is no connection yet.
 */
#include "drivers/internal.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kshell/kshell.h"
#include "wifi/wifi.h"
#include "wifi/wifi_esp32.h"

static bool s_started;
static uint8_t s_mac[6];

/* --- the module of the hardware doctor --- */

static int drv_wifi_init(void)
{
    int rc;

    if (wifi_esp32_init() != 0) {
        return -1;
    }

    if (wifi_init() != WIFI_OK) {
        return -2;
    }

    rc = wifi_start();
    if (rc != WIFI_OK) {
        kerror("wifi: the radio does not start (rc=%d)", rc);
        return -3;
    }

    (void)wifi_esp32_get_mac(s_mac);

    s_started = true;

    /*
     * The network of the last time is remembered in the NVS: the board
     * joins it on its own, without waiting for the shell.
     */
    rc = wifi_connect_saved();
    if (rc == WIFI_OK) {
        kinfo("wifi: the station joins the saved network");
    } else if (rc == WIFI_NO_CONFIG) {
        kinfo("wifi: there is no saved network yet");
    }

    return 0;
}

static int drv_wifi_check(void)
{
    struct wifi_status status;

    if (!s_started) {
        return -1;
    }

    if (wifi_get_status(&status) != WIFI_OK) {
        return -1;
    }

    switch (status.state) {
    case WIFI_STATE_CONNECTED:
        return 0;
    case WIFI_STATE_IDLE:
    case WIFI_STATE_CONNECTING:
        /* The radio is up: without a network the module is only degraded. */
        return 1;
    default:
        return -1;
    }
}

static const char *drv_wifi_detail(void)
{
    static char buf[48];
    struct wifi_status status;
    char mac[18];

    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", s_mac[0], s_mac[1], s_mac[2],
             s_mac[3], s_mac[4], s_mac[5]);

    if (!s_started || wifi_get_status(&status) != WIFI_OK) {
        snprintf(buf, sizeof(buf), "%s no radio", mac);
        return buf;
    }

    if (status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s %s", mac, status.ip);
    } else {
        snprintf(buf, sizeof(buf), "%s %s", mac, wifi_state_name(status.state));
    }

    return buf;
}

static const struct kmod s_wifi_module = {
    .name = "wifi",
    .desc = "WiFi radio (ESP32-PICO-V3-02)",
    .init = drv_wifi_init,
    .check = drv_wifi_check,
    .detail = drv_wifi_detail,
};

/* --- the command of the shell --- */

static void wifi_print_status(void)
{
    struct wifi_status status;
    char buf[96];
    char ssid[WIFI_SSID_MAX];
    int rc;

    if (wifi_get_status(&status) != WIFI_OK) {
        kshell_printf("wifi: the radio is not ready\n");
        return;
    }

    wifi_format_status(buf, sizeof(buf), &status);
    kshell_printf("%s\n", buf);

    rc = wifi_config_get(ssid, sizeof(ssid), NULL, 0);
    if (rc == WIFI_OK) {
        kshell_printf("saved network: %s\n", ssid);
    } else {
        kshell_printf("saved network: none\n");
    }
}

static void wifi_print_scan(void)
{
    struct wifi_scan_entry entries[WIFI_SCAN_MAX];
    char buf[96];
    k_size_t count = 0;
    k_size_t i;

    if (wifi_scan(entries, WIFI_SCAN_MAX, &count) != WIFI_OK) {
        kshell_printf("wifi: the scan failed\n");
        return;
    }

    if (count == 0) {
        kshell_printf("wifi: no networks around\n");
        return;
    }

    kshell_printf("%u network(s):\n", (unsigned)count);

    for (i = 0; i < count; i++) {
        wifi_format_scan_entry(buf, sizeof(buf), &entries[i]);
        kshell_printf("%s\n", buf);
    }
}

static int cmd_wifi(int argc, char **argv)
{
    const char *action = (argc > 1) ? argv[1] : "status";
    int rc;

    if (strcmp(action, "status") == 0) {
        wifi_print_status();
        return KSHELL_OK;
    }

    if (strcmp(action, "scan") == 0) {
        wifi_print_scan();
        return KSHELL_OK;
    }

    if (strcmp(action, "connect") == 0) {
        const char *ssid;
        const char *pass = NULL;

        if (argc < 3) {
            kshell_printf("usage: wifi connect <ssid> [key]\n");
            return KSHELL_USAGE;
        }

        ssid = argv[2];
        if (argc > 3) {
            pass = argv[3];
        }

        rc = wifi_connect(ssid, pass);
        if (rc != WIFI_OK) {
            kshell_printf("wifi: the connection is refused (rc=%d)\n", rc);
            return KSHELL_ERROR;
        }

        /* The network is remembered: the board joins it after a restart. */
        rc = wifi_config_save(ssid, pass);
        if (rc != WIFI_OK) {
            kshell_printf("wifi: the network is not remembered (rc=%d)\n", rc);
        }

        kshell_printf("connecting to %s\n", ssid);
        return KSHELL_OK;
    }

    if (strcmp(action, "join") == 0) {
        rc = wifi_connect_saved();
        if (rc != WIFI_OK) {
            kshell_printf("wifi: there is no network to join (rc=%d)\n", rc);
            return KSHELL_ERROR;
        }

        kshell_printf("connecting to the saved network\n");
        return KSHELL_OK;
    }

    if (strcmp(action, "disconnect") == 0) {
        rc = wifi_disconnect();
        if (rc != WIFI_OK) {
            kshell_printf("wifi: the disconnection is refused (rc=%d)\n", rc);
            return KSHELL_ERROR;
        }

        kshell_printf("disconnected\n");
        return KSHELL_OK;
    }

    if (strcmp(action, "forget") == 0) {
        rc = wifi_config_forget();
        if (rc != WIFI_OK) {
            kshell_printf("wifi: the network is not forgotten (rc=%d)\n", rc);
            return KSHELL_ERROR;
        }

        kshell_printf("the saved network is forgotten\n");
        return KSHELL_OK;
    }

    kshell_printf("usage: wifi [status|scan|connect <ssid> [key]|join|disconnect|forget]\n");

    return KSHELL_USAGE;
}

int drv_wifi_register(void)
{
    int rc = kmod_register(&s_wifi_module);

    kshell_register("wifi", "the WiFi station: status, scan, connect", cmd_wifi);

    return rc;
}
