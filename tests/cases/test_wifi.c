/*
 * Micro OS — the tests of the WiFi station.
 *
 * The radio is a table of functions, so the whole state machine of the
 * driver can be tested on the host: the fake radio below plays the part of
 * the hardware of the ESP32.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/sched.h"
#include "wifi/wifi.h"

/* --- the fake radio --- */

static int s_start_calls;
static int s_stop_calls;
static int s_connect_calls;
static int s_disconnect_calls;
static int s_scan_calls;
static int s_status_calls;
static int s_start_result;
static int s_connect_result;
static char s_last_ssid[WIFI_SSID_MAX];
static char s_last_pass[WIFI_PASS_MAX];
static struct wifi_status s_reported;
static struct wifi_scan_entry s_scan_entries[4];
static k_size_t s_scan_count;

static int fake_start(void)
{
    s_start_calls++;
    return s_start_result;
}

static int fake_stop(void)
{
    s_stop_calls++;
    return 0;
}

static int fake_connect(const char *ssid, const char *pass)
{
    s_connect_calls++;
    strncpy(s_last_ssid, ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_pass, pass != NULL ? pass : "", sizeof(s_last_pass) - 1);
    s_last_pass[sizeof(s_last_pass) - 1] = '\0';
    return s_connect_result;
}

static int fake_disconnect(void)
{
    s_disconnect_calls++;
    return 0;
}

static int fake_scan(struct wifi_scan_entry *out, k_size_t max, k_size_t *count)
{
    k_size_t i;
    k_size_t number = s_scan_count;

    s_scan_calls++;

    if (number > max) {
        number = max;
    }

    for (i = 0; i < number; i++) {
        out[i] = s_scan_entries[i];
    }

    if (count != NULL) {
        *count = number;
    }

    return 0;
}

static int fake_status(struct wifi_status *out)
{
    s_status_calls++;
    *out = s_reported;
    return 0;
}

static const struct wifi_radio s_fake_radio = {
    .start = fake_start,
    .stop = fake_stop,
    .connect = fake_connect,
    .disconnect = fake_disconnect,
    .scan = fake_scan,
    .status = fake_status,
};

/* --- the fake storage --- */

static int s_save_calls;
static int s_erase_calls;
static char s_stored_ssid[WIFI_SSID_MAX];
static char s_stored_pass[WIFI_PASS_MAX];

static int fake_load(char *ssid, k_size_t ssid_size, char *pass, k_size_t pass_size)
{
    if (s_stored_ssid[0] == '\0') {
        return -1;
    }

    strncpy(ssid, s_stored_ssid, ssid_size - 1);
    ssid[ssid_size - 1] = '\0';
    strncpy(pass, s_stored_pass, pass_size - 1);
    pass[pass_size - 1] = '\0';

    return 0;
}

static int fake_save(const char *ssid, const char *pass)
{
    s_save_calls++;
    strncpy(s_stored_ssid, ssid, sizeof(s_stored_ssid) - 1);
    s_stored_ssid[sizeof(s_stored_ssid) - 1] = '\0';
    strncpy(s_stored_pass, pass != NULL ? pass : "", sizeof(s_stored_pass) - 1);
    s_stored_pass[sizeof(s_stored_pass) - 1] = '\0';
    return 0;
}

static int fake_erase(void)
{
    s_erase_calls++;
    s_stored_ssid[0] = '\0';
    s_stored_pass[0] = '\0';
    return 0;
}

static const struct wifi_storage s_fake_storage = {
    .load = fake_load,
    .save = fake_save,
    .erase = fake_erase,
};

/* --- the tests --- */

static void reset_fake(void)
{
    s_start_calls = 0;
    s_stop_calls = 0;
    s_connect_calls = 0;
    s_disconnect_calls = 0;
    s_scan_calls = 0;
    s_status_calls = 0;
    s_start_result = 0;
    s_connect_result = 0;
    s_last_ssid[0] = '\0';
    s_last_pass[0] = '\0';
    memset(&s_reported, 0, sizeof(s_reported));
    memset(s_scan_entries, 0, sizeof(s_scan_entries));
    s_scan_count = 0;
    s_save_calls = 0;
    s_erase_calls = 0;
    s_stored_ssid[0] = '\0';
    s_stored_pass[0] = '\0';

    wifi_set_radio(&s_fake_radio);
    wifi_set_storage(&s_fake_storage);
    wifi_init();
}

static void test_the_states(void)
{
    struct wifi_status status;

    ktest_begin("wifi: the state of the station follows the radio");

    reset_fake();

    /* The radio is off until it is started. */
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_STOPPED);

    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(s_start_calls, 1);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_IDLE);

    /* Starting twice does not touch the radio again. */
    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(s_start_calls, 1);

    /* The connection is asynchronous: the radio reports the result. */
    KT_CHECK_INT_EQ(wifi_connect("home", "secret"), WIFI_OK);
    KT_CHECK_INT_EQ(s_connect_calls, 1);
    KT_CHECK_STR_EQ(s_last_ssid, "home");
    KT_CHECK_STR_EQ(s_last_pass, "secret");
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_CONNECTING);

    wifi_notify(WIFI_EVT_CONNECTED, 0);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_CONNECTED);

    KT_CHECK_INT_EQ(wifi_stop(), WIFI_OK);
    KT_CHECK_INT_EQ(s_stop_calls, 1);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_STOPPED);
}

static void test_the_failure(void)
{
    struct wifi_status status;

    ktest_begin("wifi: a disconnection during the connection is a failure");

    reset_fake();

    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(wifi_connect("home", NULL), WIFI_OK);

    wifi_notify(WIFI_EVT_DISCONNECTED, 201);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_FAILED);
    KT_CHECK_INT_EQ(status.reason, 201);

    /* The network was up and is gone: this is a disconnection, not a failure. */
    reset_fake();
    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(wifi_connect("home", NULL), WIFI_OK);
    wifi_notify(WIFI_EVT_CONNECTED, 0);
    wifi_notify(WIFI_EVT_DISCONNECTED, 3);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_DISCONNECTED);
}

static void test_the_refusals(void)
{
    struct wifi_status status;

    ktest_begin("wifi: the wrong arguments are refused");

    reset_fake();

    /* The radio is off: there is nothing to connect to. */
    KT_CHECK_INT_EQ(wifi_connect("home", NULL), WIFI_NOT_STARTED);
    KT_CHECK_INT_EQ(s_connect_calls, 0);

    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);

    KT_CHECK_INT_EQ(wifi_connect("", NULL), WIFI_INVALID);
    KT_CHECK_INT_EQ(wifi_connect(NULL, NULL), WIFI_INVALID);
    KT_CHECK_INT_EQ(s_connect_calls, 0);

    /* The name of the network is 32 characters long. */
    {
        char long_ssid[WIFI_SSID_MAX + 1];

        memset(long_ssid, 'a', sizeof(long_ssid) - 1);
        long_ssid[sizeof(long_ssid) - 1] = '\0';

        KT_CHECK_INT_EQ(wifi_connect(long_ssid, NULL), WIFI_INVALID);
        KT_CHECK_INT_EQ(s_connect_calls, 0);
    }

    /* The radio itself refuses: the state says that it failed. */
    s_connect_result = -1;
    KT_CHECK_INT_EQ(wifi_connect("home", NULL), WIFI_ERROR);
    KT_CHECK_INT_EQ(wifi_get_status(&status), WIFI_OK);
    KT_CHECK_INT_EQ(status.state, WIFI_STATE_FAILED);
}

static void test_the_configuration(void)
{
    char ssid[WIFI_SSID_MAX];

    ktest_begin("wifi: the network is remembered between the restarts");

    reset_fake();

    /* There is nothing to remember yet. */
    KT_CHECK_INT_EQ(wifi_config_load(), WIFI_NO_CONFIG);
    KT_CHECK_INT_EQ(wifi_config_get(ssid, sizeof(ssid), NULL, 0), WIFI_NO_CONFIG);

    KT_CHECK_INT_EQ(wifi_config_save("home", "secret"), WIFI_OK);
    KT_CHECK_INT_EQ(s_save_calls, 1);
    KT_CHECK_STR_EQ(s_stored_ssid, "home");
    KT_CHECK_STR_EQ(s_stored_pass, "secret");

    /* A new core reads the network back from the storage. */
    wifi_init();
    KT_CHECK_INT_EQ(wifi_config_load(), WIFI_OK);
    KT_CHECK_INT_EQ(wifi_config_get(ssid, sizeof(ssid), NULL, 0), WIFI_OK);
    KT_CHECK_STR_EQ(ssid, "home");

    /* The saved network is the one the station joins. */
    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(wifi_connect_saved(), WIFI_OK);
    KT_CHECK_STR_EQ(s_last_ssid, "home");
    KT_CHECK_STR_EQ(s_last_pass, "secret");

    KT_CHECK_INT_EQ(wifi_config_forget(), WIFI_OK);
    KT_CHECK_INT_EQ(s_erase_calls, 1);
    KT_CHECK_INT_EQ(wifi_config_load(), WIFI_NO_CONFIG);
}

static void test_the_scan(void)
{
    struct wifi_scan_entry entries[WIFI_SCAN_MAX];
    k_size_t count = 0;

    ktest_begin("wifi: the scan reports the networks around");

    reset_fake();

    /* The radio is off: there is nothing to scan. */
    KT_CHECK_INT_EQ(wifi_scan(entries, WIFI_SCAN_MAX, &count), WIFI_NOT_STARTED);

    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);

    strcpy(s_scan_entries[0].ssid, "home");
    strcpy(s_scan_entries[0].bssid, "aa:bb:cc:dd:ee:ff");
    s_scan_entries[0].rssi = -42;
    s_scan_entries[0].channel = 6;
    s_scan_entries[0].secure = true;

    strcpy(s_scan_entries[1].ssid, "guest");
    s_scan_entries[1].rssi = -70;
    s_scan_entries[1].channel = 11;
    s_scan_entries[1].secure = false;

    s_scan_count = 2;

    KT_CHECK_INT_EQ(wifi_scan(entries, WIFI_SCAN_MAX, &count), WIFI_OK);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_STR_EQ(entries[0].ssid, "home");
    KT_CHECK_INT_EQ(entries[0].rssi, -42);
    KT_CHECK_INT_EQ(entries[0].channel, 6);
    KT_CHECK(entries[0].secure);
    KT_CHECK_STR_EQ(entries[1].ssid, "guest");
    KT_CHECK(!entries[1].secure);
}

static void test_the_format(void)
{
    struct wifi_status status;
    struct wifi_scan_entry entry;
    char buf[96];

    ktest_begin("wifi: the status and the networks are readable");

    reset_fake();

    memset(&status, 0, sizeof(status));
    status.state = WIFI_STATE_CONNECTED;
    strcpy(status.ssid, "home");
    strcpy(status.bssid, "aa:bb:cc:dd:ee:ff");
    strcpy(status.ip, "192.168.1.42");
    status.rssi = -42;
    status.channel = 6;

    wifi_format_status(buf, sizeof(buf), &status);
    KT_CHECK(strstr(buf, "home") != NULL);
    KT_CHECK(strstr(buf, "192.168.1.42") != NULL);
    KT_CHECK(strstr(buf, "-42") != NULL);

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.ssid, "guest");
    entry.rssi = -70;
    entry.channel = 11;
    entry.secure = true;

    wifi_format_scan_entry(buf, sizeof(buf), &entry);
    KT_CHECK(strstr(buf, "guest") != NULL);
    KT_CHECK(strstr(buf, "-70") != NULL);
    KT_CHECK(strstr(buf, "locked") != NULL);
}

/*
 * The property: a network that goes away is asked for again by itself (the
 * router was rebooted), the attempts are few and further apart every time,
 * and a station that was told to leave stays where it is.
 */
static void test_the_reconnection(void)
{
    k_u32_t first;
    k_u32_t second;

    ktest_begin("wifi: the station comes back to the network by itself");

    reset_fake();

    KT_CHECK_INT_EQ(wifi_start(), WIFI_OK);
    KT_CHECK_INT_EQ(wifi_connect("home", "secret"), WIFI_OK);

    /* The network is up, and then the router is rebooted. */
    wifi_notify(WIFI_EVT_CONNECTED, 0);
    wifi_notify(WIFI_EVT_DISCONNECTED, 0);

    first = wifi_reconnect_after_ms();
    KT_CHECK(first > 0);

    /* One attempt is planned at a time: the next event adds nothing. */
    KT_CHECK_INT_EQ((int)wifi_reconnect_after_ms(), 0);

    /*
     * The wait grows with every attempt: the second wait is longer than
     * the first. The test sleeps a little longer than the first one.
     */
    k_sched_sleep_ms(first + 100u);
    second = wifi_reconnect_after_ms();
    KT_CHECK(second > first);

    /* A connection asked for by hand starts the count of the attempts again. */
    KT_CHECK_INT_EQ(wifi_connect("home", "secret"), WIFI_OK);
    wifi_notify(WIFI_EVT_CONNECTED, 0);
    wifi_notify(WIFI_EVT_DISCONNECTED, 0);
    KT_CHECK_INT_EQ((int)wifi_reconnect_after_ms(), (int)first);

    /* A station that was told to leave does not come back by itself. */
    KT_CHECK_INT_EQ(wifi_disconnect(), WIFI_OK);
    wifi_notify(WIFI_EVT_DISCONNECTED, 0);
    KT_CHECK_INT_EQ((int)wifi_reconnect_after_ms(), 0);
}

void test_wifi(void)
{
    test_the_states();
    test_the_failure();
    test_the_refusals();
    test_the_configuration();
    test_the_scan();
    test_the_format();
    test_the_reconnection();
}
