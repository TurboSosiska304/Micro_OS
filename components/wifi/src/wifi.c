/*
 * Micro OS — the WiFi station: the portable core of the driver.
 *
 * The core owns the state of the station and the configuration of the
 * network. The hardware is behind the radio: a table of functions that the
 * backend fills in. Everything here is plain C11, so the logic of the driver
 * is covered by the unit tests of the host.
 */
#include "wifi/wifi.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"

static const struct wifi_radio *s_radio;
static const struct wifi_storage *s_storage;
static struct wifi_status s_status;
static char s_ssid[WIFI_SSID_MAX];
static char s_pass[WIFI_PASS_MAX];
static bool s_started;

/*
 * The station goes back to the network by itself after a drop: a router
 * that was rebooted must not need a hand every time. The policy lives
 * here - how many times to try, how long to wait and when to give up -
 * and the timer that wakes the radio up is the port's, because a
 * portable core has no clock of its own to sleep on.
 */
#define WIFI_RECONNECT_MAX 5        /* the attempts of one connection */
#define WIFI_RECONNECT_BASE_MS 1000 /* the wait before the first attempt */
#define WIFI_RECONNECT_MAX_MS 30000 /* the longest wait between attempts */

static struct {
    bool wanted;     /* the station was asked to be on a network */
    int attempts;    /* the attempts made for the connection in progress */
    k_u64_t next_ms; /* the moment the next attempt may start */
    /*
     * The network to go back to. It is the last one that was asked for,
     * saved in the NVS or not: the cache of the configuration (s_ssid,
     * s_pass) is the one of the records, and a network that was joined by
     * hand has to be remembered here.
     */
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
} s_reconnect;

/* The attempts of a connection are forgotten when the connection is made. */
static void reconnect_forget(void)
{
    s_reconnect.attempts = 0;
    s_reconnect.next_ms = 0;
}

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

void wifi_set_radio(const struct wifi_radio *radio)
{
    s_radio = radio;
}

void wifi_set_storage(const struct wifi_storage *storage)
{
    s_storage = storage;
}

int wifi_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WIFI_STATE_STOPPED;

    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_started = false;

    memset(&s_reconnect, 0, sizeof(s_reconnect));

    return WIFI_OK;
}

int wifi_start(void)
{
    if (s_radio == NULL || s_radio->start == NULL) {
        return WIFI_NO_RADIO;
    }

    if (s_started) {
        return WIFI_OK;
    }

    if (s_radio->start() != 0) {
        kerror("wifi: the radio does not start");
        return WIFI_ERROR;
    }

    s_started = true;
    s_status.state = WIFI_STATE_IDLE;
    s_status.reason = 0;

    return WIFI_OK;
}

int wifi_stop(void)
{
    if (s_radio == NULL || s_radio->stop == NULL) {
        return WIFI_NO_RADIO;
    }

    if (!s_started) {
        return WIFI_OK;
    }

    if (s_radio->stop() != 0) {
        kerror("wifi: the radio does not stop");
        return WIFI_ERROR;
    }

    s_started = false;
    s_reconnect.wanted = false;
    reconnect_forget();
    s_status.state = WIFI_STATE_STOPPED;
    s_status.ssid[0] = '\0';
    s_status.bssid[0] = '\0';
    s_status.ip[0] = '\0';
    s_status.rssi = 0;
    s_status.channel = 0;

    return WIFI_OK;
}

int wifi_connect(const char *ssid, const char *pass)
{
    int rc;

    if (s_radio == NULL || s_radio->connect == NULL) {
        return WIFI_NO_RADIO;
    }

    if (!s_started) {
        return WIFI_NOT_STARTED;
    }

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= WIFI_SSID_MAX) {
        return WIFI_INVALID;
    }

    if (pass != NULL && strlen(pass) >= WIFI_PASS_MAX) {
        return WIFI_INVALID;
    }

    /*
     * A connection asked for by hand starts the attempts of the policy
     * again: whatever the radio was doing before, the user wants this
     * network now.
     */
    s_reconnect.wanted = true;
    reconnect_forget();
    copy_str(s_reconnect.ssid, sizeof(s_reconnect.ssid), ssid);
    copy_str(s_reconnect.pass, sizeof(s_reconnect.pass), pass);

    rc = s_radio->connect(ssid, pass);
    if (rc != 0) {
        /* The radio refused the request: the network is not there. */
        s_status.state = WIFI_STATE_FAILED;
        s_status.reason = rc;
        return WIFI_ERROR;
    }

    /*
     * The association is asynchronous: the radio reports the result later,
     * as an event of the core.
     */
    s_status.state = WIFI_STATE_CONNECTING;
    s_status.reason = 0;
    copy_str(s_status.ssid, sizeof(s_status.ssid), ssid);

    return WIFI_OK;
}

int wifi_disconnect(void)
{
    if (s_radio == NULL || s_radio->disconnect == NULL) {
        return WIFI_NO_RADIO;
    }

    if (!s_started) {
        return WIFI_NOT_STARTED;
    }

    if (s_radio->disconnect() != 0) {
        return WIFI_ERROR;
    }

    /*
     * The station was asked to leave: nobody has to bring it back by
     * itself until a connection is asked for again.
     */
    s_reconnect.wanted = false;
    reconnect_forget();

    return WIFI_OK;
}

int wifi_scan(struct wifi_scan_entry *out, k_size_t max, k_size_t *count)
{
    if (s_radio == NULL || s_radio->scan == NULL) {
        return WIFI_NO_RADIO;
    }

    if (!s_started) {
        return WIFI_NOT_STARTED;
    }

    if (out == NULL || max == 0) {
        return WIFI_INVALID;
    }

    return s_radio->scan(out, max, count);
}

/* Take the live data of the radio: the address and the signal. */
static void refresh_live_data(void)
{
    struct wifi_status live;

    if (s_radio == NULL || s_radio->status == NULL) {
        return;
    }

    memset(&live, 0, sizeof(live));

    if (s_radio->status(&live) != 0) {
        return;
    }

    k_port_enter_critical();
    copy_str(s_status.ssid, sizeof(s_status.ssid), live.ssid);
    copy_str(s_status.bssid, sizeof(s_status.bssid), live.bssid);
    copy_str(s_status.ip, sizeof(s_status.ip), live.ip);
    s_status.rssi = live.rssi;
    s_status.channel = live.channel;
    k_port_exit_critical();
}

int wifi_get_status(struct wifi_status *out)
{
    if (out == NULL) {
        return WIFI_INVALID;
    }

    if (s_started) {
        refresh_live_data();
    }

    k_port_enter_critical();
    memcpy(out, &s_status, sizeof(*out));
    k_port_exit_critical();

    return WIFI_OK;
}

int wifi_config_save(const char *ssid, const char *pass)
{
    if (s_storage == NULL || s_storage->save == NULL) {
        return WIFI_NO_STORAGE;
    }

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= WIFI_SSID_MAX) {
        return WIFI_INVALID;
    }

    if (pass != NULL && strlen(pass) >= WIFI_PASS_MAX) {
        return WIFI_INVALID;
    }

    if (s_storage->save(ssid, pass != NULL ? pass : "") != 0) {
        kerror("wifi: the configuration is not saved");
        return WIFI_ERROR;
    }

    copy_str(s_ssid, sizeof(s_ssid), ssid);
    copy_str(s_pass, sizeof(s_pass), pass != NULL ? pass : "");

    return WIFI_OK;
}

int wifi_config_load(void)
{
    if (s_storage == NULL || s_storage->load == NULL) {
        return WIFI_NO_STORAGE;
    }

    if (s_storage->load(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)) != 0) {
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        return WIFI_NO_CONFIG;
    }

    return WIFI_OK;
}

int wifi_config_forget(void)
{
    int rc = WIFI_OK;

    if (s_storage == NULL || s_storage->erase == NULL) {
        return WIFI_NO_STORAGE;
    }

    if (s_storage->erase() != 0) {
        rc = WIFI_ERROR;
    }

    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    return rc;
}

int wifi_config_get(char *ssid, k_size_t ssid_size, char *pass, k_size_t pass_size)
{
    if (ssid == NULL || ssid_size == 0) {
        return WIFI_INVALID;
    }

    copy_str(ssid, ssid_size, s_ssid);

    if (pass != NULL && pass_size > 0) {
        copy_str(pass, pass_size, s_pass);
    }

    return (s_ssid[0] != '\0') ? WIFI_OK : WIFI_NO_CONFIG;
}

int wifi_connect_saved(void)
{
    int rc = wifi_config_load();

    if (rc != WIFI_OK) {
        return rc;
    }

    return wifi_connect(s_ssid, s_pass);
}

k_u32_t wifi_reconnect_after_ms(void)
{
    k_u64_t now;
    k_u32_t wait;

    if (!s_reconnect.wanted || s_reconnect.ssid[0] == '\0') {
        return 0;
    }

    if (s_reconnect.attempts >= WIFI_RECONNECT_MAX) {
        /* The network was asked for WIFI_RECONNECT_MAX times: it is not there. */
        return 0;
    }

    now = k_port_uptime_ms();

    /*
     * A drop is often reported by several events: only one attempt is
     * planned at a time, so that the radio is not woken up in a pile.
     */
    if (s_reconnect.next_ms > now) {
        return 0;
    }

    /*
     * The wait grows with every attempt: one second, then two, then four
     * and so on, up to the longest wait of the policy.
     */
    wait = WIFI_RECONNECT_BASE_MS << ((s_reconnect.attempts < 5) ? s_reconnect.attempts : 5);
    if (wait > WIFI_RECONNECT_MAX_MS) {
        wait = WIFI_RECONNECT_MAX_MS;
    }

    s_reconnect.attempts++;
    s_reconnect.next_ms = now + wait;

    return wait;
}

int wifi_reconnect(void)
{
    if (!s_reconnect.wanted || s_reconnect.ssid[0] == '\0') {
        return WIFI_NO_CONFIG;
    }

    if (s_radio == NULL || s_radio->connect == NULL) {
        return WIFI_NO_RADIO;
    }

    if (!s_started) {
        return WIFI_NOT_STARTED;
    }

    /*
     * The count of the attempts is not restarted here: it is what stops
     * the station from asking a network with a wrong key all day.
     */
    if (s_radio->connect(s_reconnect.ssid, s_reconnect.pass) != 0) {
        s_status.state = WIFI_STATE_FAILED;
        return WIFI_ERROR;
    }

    s_status.state = WIFI_STATE_CONNECTING;

    return WIFI_OK;
}

void wifi_notify(int event, int reason)
{
    k_port_enter_critical();

    switch (event) {
    case WIFI_EVT_STARTED:
        s_status.state = WIFI_STATE_IDLE;
        s_status.reason = 0;
        break;
    case WIFI_EVT_CONNECTED:
        s_status.state = WIFI_STATE_CONNECTED;
        s_status.reason = 0;
        /* The network is there: the attempts of the policy are over. */
        s_reconnect.attempts = 0;
        s_reconnect.next_ms = 0;
        break;
    case WIFI_EVT_DISCONNECTED:
        /*
         * A disconnection while the station was connecting means that the
         * attempt failed: the network was not there or the key was wrong.
         */
        if (s_status.state == WIFI_STATE_CONNECTING) {
            s_status.state = WIFI_STATE_FAILED;
        } else {
            s_status.state = WIFI_STATE_DISCONNECTED;
        }
        s_status.reason = reason;
        break;
    case WIFI_EVT_FAILED:
        s_status.state = WIFI_STATE_FAILED;
        s_status.reason = reason;
        break;
    default:
        k_port_exit_critical();
        return;
    }

    k_port_exit_critical();

    if (event == WIFI_EVT_CONNECTED) {
        /* The network is up: the address and the signal are known now. */
        refresh_live_data();
    }
}

const char *wifi_state_name(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_STOPPED:
        return "stopped";
    case WIFI_STATE_IDLE:
        return "idle";
    case WIFI_STATE_CONNECTING:
        return "connecting";
    case WIFI_STATE_CONNECTED:
        return "connected";
    case WIFI_STATE_DISCONNECTED:
        return "disconnected";
    case WIFI_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

int wifi_format_status(char *buf, k_size_t size, const struct wifi_status *status)
{
    int written;

    if (buf == NULL || size == 0) {
        return 0;
    }

    if (status == NULL) {
        copy_str(buf, size, "");
        return 0;
    }

    switch (status->state) {
    case WIFI_STATE_CONNECTED:
        written = snprintf(buf, size, "%s %s, ip %s, %d dBm, ch %d", status->ssid, status->bssid,
                           status->ip, status->rssi, status->channel);
        break;
    case WIFI_STATE_CONNECTING:
        written = snprintf(buf, size, "%s, connecting", status->ssid);
        break;
    case WIFI_STATE_FAILED:
        written = snprintf(buf, size, "failed, reason %d", status->reason);
        break;
    case WIFI_STATE_DISCONNECTED:
        written = snprintf(buf, size, "disconnected, reason %d", status->reason);
        break;
    case WIFI_STATE_IDLE:
        written = snprintf(buf, size, "idle");
        break;
    case WIFI_STATE_STOPPED:
    default:
        written = snprintf(buf, size, "stopped");
        break;
    }

    if (written < 0) {
        return 0;
    }

    buf[(k_size_t)written < size ? (k_size_t)written : size - 1] = '\0';

    return written;
}

int wifi_format_scan_entry(char *buf, k_size_t size, const struct wifi_scan_entry *entry)
{
    int written;

    if (buf == NULL || size == 0) {
        return 0;
    }

    if (entry == NULL) {
        copy_str(buf, size, "");
        return 0;
    }

    written = snprintf(buf, size, "%-16s %5d dBm  ch %2d  %s", entry->ssid, entry->rssi,
                       entry->channel, entry->secure ? "locked" : "open");

    if (written < 0) {
        return 0;
    }

    buf[(k_size_t)written < size ? (k_size_t)written : size - 1] = '\0';

    return written;
}
