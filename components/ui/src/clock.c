/*
 * Micro OS — the clock of the panel: the format of the theme and the hour of
 * the machine (ui/clock.h).
 *
 * The arithmetic is the one of a wall clock and nothing else: the seconds of
 * the day of a zone, then the two or three numbers a person reads. There is no
 * calendar here - the panel shows the hour, and a date of the desk is a second
 * line of the panel of a later version.
 */
#include "ui/clock.h"

#include <string.h>

#define CLOCK_DAY_SECONDS 86400L
#define CLOCK_MINUTE_SECONDS 60L
#define CLOCK_HOUR_SECONDS 3600L

void ui_clock_default(struct ui_clock *clock)
{
    if (clock == NULL) {
        return;
    }

    clock->twelve = false;
    clock->offset_minutes = 0;
}

/* The name and the value of a line `key=value`; false when there is no `=`. */
static bool split_line(const char *line, const char **key, k_size_t *key_len, const char **value)
{
    const char *equals;

    if (line == NULL) {
        return false;
    }

    equals = strchr(line, '=');
    if (equals == NULL) {
        return false;
    }

    *key = line;
    *key_len = (k_size_t)(equals - line);
    *value = equals + 1;

    return true;
}

static bool key_is(const char *key, k_size_t key_len, const char *name)
{
    k_size_t length = strlen(name);

    return key_len == length && strncmp(key, name, length) == 0;
}

/* A number with an optional sign, the whole value and nothing else. */
static bool parse_int(const char *text, int *out)
{
    int sign = 1;
    int value = 0;
    k_size_t at = 0;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (text[0] == '-' || text[0] == '+') {
        sign = text[0] == '-' ? -1 : 1;
        at = 1;
    }

    if (text[at] == '\0') {
        return false;
    }

    for (; text[at] != '\0'; at++) {
        if (text[at] < '0' || text[at] > '9') {
            return false;
        }

        value = value * 10 + (text[at] - '0');
        if (value > 100000) {
            return false; /* a number a zone is not; the value is not a clock */
        }
    }

    *out = sign * value;

    return true;
}

/* The value without the newline or the trailing spaces of a line of a file. */
static k_size_t trim_length(const char *value)
{
    k_size_t length = strlen(value);

    while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' ||
                          value[length - 1] == ' ' || value[length - 1] == '\t')) {
        length--;
    }

    return length;
}

bool ui_clock_line(struct ui_clock *clock, const char *line)
{
    const char *key;
    const char *value;
    k_size_t key_len;
    k_size_t length;

    if (clock == NULL || !split_line(line, &key, &key_len, &value)) {
        return false;
    }

    length = trim_length(value);

    if (key_is(key, key_len, "clock")) {
        if (length == 3u && strncmp(value, "12h", 3u) == 0) {
            clock->twelve = true;
        } else if (length == 3u && strncmp(value, "24h", 3u) == 0) {
            clock->twelve = false;
        }

        return true;
    }

    if (key_is(key, key_len, "utc_offset")) {
        char number[12];
        int minutes;

        if (length >= sizeof(number)) {
            return true;
        }

        memcpy(number, value, length);
        number[length] = '\0';

        if (parse_int(number, &minutes) && minutes >= UI_CLOCK_OFFSET_MIN &&
            minutes <= UI_CLOCK_OFFSET_MAX) {
            clock->offset_minutes = minutes;
        }

        return true;
    }

    return false;
}

/* Two digits of a number below 100, or the three of the hour of the twelve of
 * a clock. The library has no formatter of its own: a clock is twelve bytes
 * of text and a loop of digits, which is smaller than the call. */
static k_size_t put_number(char *out, k_size_t at, k_size_t size, long value, int digits)
{
    int place;

    for (place = digits - 1; place >= 0; place--) {
        long divisor = 1;
        int step;

        for (step = 0; step < place; step++) {
            divisor *= 10;
        }

        if (at + 1u < size) {
            out[at] = (char)('0' + (int)((value / divisor) % 10));
        }

        at++;
    }

    return at;
}

static k_size_t put_text(char *out, k_size_t at, k_size_t size, const char *text)
{
    while (*text != '\0') {
        if (at + 1u < size) {
            out[at] = *text;
        }

        at++;
        text++;
    }

    return at;
}

void ui_clock_text(const struct ui_clock *clock, long seconds, char *out, k_size_t size)
{
    struct ui_clock fallback;
    long local;
    long hour;
    long minute;
    long second;
    k_size_t at = 0;
    bool pm = false;

    if (out == NULL || size == 0) {
        return;
    }

    if (clock == NULL) {
        ui_clock_default(&fallback);
        clock = &fallback;
    }

    if (clock->offset_minutes != 0) {
        seconds += (long)clock->offset_minutes * CLOCK_MINUTE_SECONDS;
    }

    local = seconds % CLOCK_DAY_SECONDS;
    if (local < 0) {
        local += CLOCK_DAY_SECONDS;
    }

    hour = local / CLOCK_HOUR_SECONDS;
    minute = (local % CLOCK_HOUR_SECONDS) / CLOCK_MINUTE_SECONDS;
    second = local % CLOCK_MINUTE_SECONDS;

    if (clock->twelve) {
        pm = hour >= 12;
        hour = hour % 12;
        if (hour == 0) {
            hour = 12;
        }

        at = put_number(out, at, size, hour, hour >= 10 ? 2 : 1);
    } else {
        at = put_number(out, at, size, hour, 2);
    }

    at = put_text(out, at, size, ":");
    at = put_number(out, at, size, minute, 2);
    at = put_text(out, at, size, ":");
    at = put_number(out, at, size, second, 2);

    if (clock->twelve) {
        at = put_text(out, at, size, pm ? " PM" : " AM");
    }

    out[at < size ? at : size - 1u] = '\0';
}
