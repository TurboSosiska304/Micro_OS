/*
 * Micro OS — the commands of the board on the host.
 *
 * The shell of the board owns these commands because the drivers register
 * them, and a person at the simulator expects the same vocabulary: `date`,
 * `battery`, `beep`, `led`, `input`, `wifi`. None of them can touch hardware
 * here, so each says what the host really is instead of pretending: a fixed
 * battery, no buzzer, no LED, no radio, buttons from a script.
 *
 * `date set` moves an offset from the clock of the process rather than the
 * clock itself, so `ntp` and the command share one notion of "now" and the
 * rest of the system keeps running on the same monotonic time.
 */
#include "host_stubs.h"
#include "port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/clock.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kshell/kshell.h"

/* The wait between two events of the input script. */
#define HOST_INPUT_DELAY_MS 200

/* --- the clock --- */

static bool digits(const char *text, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }

    return true;
}

static int number(const char *text)
{
    return (text[0] - '0') * 10 + (text[1] - '0');
}

/*
 * "YYYY-MM-DD" and "HH:MM:SS" as the seconds of Unix (UTC). The calendar is
 * checked by asking the C library to turn the moment back and compare it:
 * timegm() accepts 2026-02-30 by moving it to March, and the shell must not.
 */
static bool parse_moment(const char *date, const char *time, k_i64_t *out)
{
    struct tm moment;
    struct tm check;
    time_t seconds;
    int year;
    int month;
    int day;

    if (date == NULL || time == NULL || strlen(date) != 10 || strlen(time) != 8) {
        return false;
    }

    if (!digits(date, 4) || date[4] != '-' || !digits(date + 5, 2) || date[7] != '-' ||
        !digits(date + 8, 2)) {
        return false;
    }

    if (!digits(time, 2) || time[2] != ':' || !digits(time + 3, 2) || time[5] != ':' ||
        !digits(time + 6, 2)) {
        return false;
    }

    year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
    month = number(date + 5);
    day = number(date + 8);

    /* The calendar of the board stops at 2099, and Unix time starts at 1970. */
    if (year < 1970 || year > 2099) {
        return false;
    }

    memset(&moment, 0, sizeof(moment));
    moment.tm_year = year - 1900;
    moment.tm_mon = month - 1;
    moment.tm_mday = day;
    moment.tm_hour = number(time);
    moment.tm_min = number(time + 3);
    moment.tm_sec = number(time + 6);
    moment.tm_isdst = 0;

    /*
     * timegm() normalises its argument in place (February 30 becomes March
     * 2), so the moment is kept aside and compared with what the calendar
     * gave back: a day that does not exist is not a date.
     */
    check = moment;
    seconds = timegm(&check);
    if (seconds == (time_t)-1 || gmtime_r(&seconds, &check) == NULL) {
        return false;
    }

    if (check.tm_year != moment.tm_year || check.tm_mon != moment.tm_mon ||
        check.tm_mday != moment.tm_mday || check.tm_hour != moment.tm_hour ||
        check.tm_min != moment.tm_min || check.tm_sec != moment.tm_sec) {
        return false;
    }

    *out = (k_i64_t)seconds;
    return true;
}

static int cmd_date(int argc, char **argv)
{
    char line[32];
    k_i64_t moment;

    if (argc == 1) {
        host_clock_text(line, sizeof(line));
        kshell_printf("%s\n", line);
        return KSHELL_OK;
    }

    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        if (!parse_moment(argv[2], argv[3], &moment)) {
            kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
            return KSHELL_USAGE;
        }

        if (host_clock_set_unix((k_u64_t)moment) != 0) {
            kshell_printf("date set: the clock did not take the time\n");
            return KSHELL_ERROR;
        }

        host_clock_text(line, sizeof(line));
        kshell_printf("%s\n", line);
        return KSHELL_OK;
    }

    kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
    return KSHELL_USAGE;
}

/* --- the pieces that are simply not there --- */

static int cmd_battery(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("battery: fixed 4.0 V, 100%% (the host has no battery)\n");
    return KSHELL_OK;
}

static int cmd_beep(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("beep: the host has no buzzer\n");
    return KSHELL_OK;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        kshell_printf("usage: led on|off|heart|dance\n");
        return KSHELL_USAGE;
    }

    kshell_printf("led: the host has no led\n");
    return KSHELL_OK;
}

/* --- the buttons from a script --- */

/* Cut the newline and the spaces around one line of the script. */
static char *trim_line(char *line)
{
    size_t len;

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    return line;
}

static int cmd_input(int argc, char **argv)
{
    unsigned long seconds = 10;
    const char *path;
    char line[64];
    k_u64_t start;
    FILE *script;

    if (argc > 1) {
        char *end = NULL;

        seconds = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || seconds == 0 || seconds > 3600) {
            kshell_printf("usage: input [seconds]\n");
            return KSHELL_USAGE;
        }
    }

    path = host_input_script();
    if (path == NULL) {
        kshell_printf("input: no script ($MICROOS_INPUT is not set)\n");
        return KSHELL_ERROR;
    }

    script = fopen(path, "r");
    if (script == NULL) {
        kshell_printf("input: cannot open the script %s\n", path);
        return KSHELL_ERROR;
    }

    kshell_printf("input: playing %s (%lu s)\n", path, seconds);
    start = k_sysinfo_uptime_ms();

    while (fgets(line, sizeof(line), script) != NULL) {
        char *event = trim_line(line);
        k_u64_t elapsed = k_sysinfo_uptime_ms() - start;

        if (event[0] == '\0' || event[0] == '#') {
            continue;
        }

        if (elapsed >= (k_u64_t)seconds * 1000u) {
            break;
        }

        kshell_printf("%s at %llu ms\n", event, (unsigned long long)elapsed);
        k_sched_sleep_ms(HOST_INPUT_DELAY_MS);
    }

    fclose(script);
    return KSHELL_OK;
}

/* --- the radio that is not there --- */

static int cmd_wifi(int argc, char **argv)
{
    const char *action = (argc > 1) ? argv[1] : "status";

    if (strcmp(action, "status") == 0 || strcmp(action, "scan") == 0 ||
        strcmp(action, "connect") == 0 || strcmp(action, "join") == 0 ||
        strcmp(action, "disconnect") == 0 || strcmp(action, "forget") == 0) {
        kshell_printf("wifi: no radio (the host has none)\n");
        return KSHELL_OK;
    }

    kshell_printf("usage: wifi [status|scan|connect <ssid> [key]|join|disconnect|forget]\n");
    return KSHELL_USAGE;
}

void host_commands_register(void)
{
    (void)kshell_register("date", "the time of the host clock", cmd_date);
    (void)kshell_register("battery", "the battery: voltage and percent", cmd_battery);
    (void)kshell_register("beep", "sound the buzzer: beep [frequency] [ms]", cmd_beep);
    (void)kshell_register("led", "the red led: led on|off|heart|dance", cmd_led);
    (void)kshell_register("input", "the buttons: input [seconds]", cmd_input);
    (void)kshell_register("wifi", "the WiFi station: status, scan, connect", cmd_wifi);
}
