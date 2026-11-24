/*
 * Micro OS — the reverse of `strftime`.
 *
 * `strptime` reads a moment of the calendar out of the text a formatter
 * printed: the conversions are the ones of `strftime` of this library
 * (`src/time.c`), the names of the days and the months of the C locale, and
 * the numbers of `%d`, `%H`, `%Y` and the rest. The fields a format does not
 * name stay as the caller left them, which is what POSIX promises; `%C` and
 * `%y` therefore combine only when both are in the format, and `%I` with `%p`
 * decides the hour at the end of the scan.
 *
 * Whitespace in the format matches any run of whitespace in the text, as it
 * does in `scanf`; a literal byte has to match itself, and a conversion that
 * does not match ends the scan with NULL. A conversion this reader does not
 * know is a mistake of the format and answers `EINVAL`, not a silent zero.
 *
 * The names are matched without the case of a letter and the longest name
 * wins, so `January` and `Jan` both read `%B`, and `%b` reads either - the
 * library is more generous than the letter, which is what the readers of the
 * real world do.
 */
#include <errno.h>
#include <string.h>
#include <time.h>

static const char *const g_day_short_s[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const g_day_long_s[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};
static const char *const g_month_short_s[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const g_month_long_s[] = {"January",   "February", "March",    "April",
                                             "May",       "June",     "July",     "August",
                                             "September", "October",  "November", "December"};

static int scan_space(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static int scan_digit(int byte)
{
    return byte >= '0' && byte <= '9';
}

static void skip_space(const char **cursor)
{
    while (scan_space((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

/* Does `text` begin with `word`, without the case of a letter? */
static int word_prefix(const char *text, const char *word)
{
    while (*word != '\0') {
        int left = (unsigned char)*text++;
        int right = (unsigned char)*word++;

        if (left >= 'A' && left <= 'Z') {
            left += 'a' - 'A';
        }
        if (right >= 'A' && right <= 'Z') {
            right += 'a' - 'A';
        }
        if (left != right) {
            return 0;
        }
    }

    return 1;
}

/*
 * Read the longest of the names at the cursor: the short table and the long
 * one are one set of alternatives and the index of the match is the field.
 */
static int name_read(const char **cursor, const char *const short_names[],
                     const char *const long_names[], int count, int *found)
{
    size_t best_length = 0;
    int best = -1;
    int at;

    for (at = 0; at < count; at++) {
        size_t length = strlen(short_names[at]);

        if (length > best_length && word_prefix(*cursor, short_names[at])) {
            best = at;
            best_length = length;
        }
    }
    for (at = 0; at < count; at++) {
        size_t length = strlen(long_names[at]);

        if (length > best_length && word_prefix(*cursor, long_names[at])) {
            best = at;
            best_length = length;
        }
    }

    if (best < 0) {
        return -1;
    }

    *cursor += best_length;
    *found = best;

    return 0;
}

/*
 * A number of at most `width` digits. The leading whitespace of the field is
 * part of the padding of `%e` and not of the number, and it is skipped here
 * as the readers of the real world do.
 */
static int number_read(const char **cursor, int width, int *value)
{
    const char *at = *cursor;
    int number = 0;
    int digits = 0;

    while (scan_space((unsigned char)*at)) {
        at++;
    }
    while (digits < width && scan_digit((unsigned char)*at)) {
        number = number * 10 + (*at - '0');
        at++;
        digits++;
    }

    if (digits == 0) {
        return -1;
    }

    *cursor = at;
    *value = number;

    return 0;
}

static int in_range(int value, int low, int high)
{
    return value >= low && value <= high;
}

/* The zone of `%z`: `Z`, or a sign and the hours and the minutes. The offset
 * names no second clock of this machine and is checked and dropped. */
static int zone_read(const char **cursor)
{
    const char *at = *cursor;
    int digits = 0;

    if (*at == 'Z' || *at == 'z') {
        *cursor = at + 1;
        return 0;
    }
    if (*at != '+' && *at != '-') {
        return -1;
    }
    at++;

    while (digits < 2 && scan_digit((unsigned char)*at)) {
        at++;
        digits++;
    }
    if (digits != 2) {
        return -1;
    }
    if (*at == ':') {
        at++;
    }
    digits = 0;
    while (digits < 2 && scan_digit((unsigned char)*at)) {
        at++;
        digits++;
    }
    if (digits != 2) {
        return -1;
    }

    *cursor = at;

    return 0;
}

/* The name of the zone of `%Z`, a run of letters that this machine ignores:
 * there is one zone and it is UTC. */
static int zone_name_read(const char **cursor)
{
    const char *at = *cursor;

    while ((*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z')) {
        at++;
    }

    if (at == *cursor) {
        return -1;
    }

    *cursor = at;

    return 0;
}

char *strptime(const char *text, const char *format, struct tm *parts)
{
    const char *in = text;
    int hour12 = -1;
    int afternoon = -1;
    int century = -1;

    if (text == NULL || format == NULL || parts == NULL) {
        errno = EINVAL;
        return NULL;
    }

    while (*format != '\0') {
        char specifier;
        int value;
        const char *rest;

        if (scan_space((unsigned char)*format)) {
            skip_space(&in);
            while (scan_space((unsigned char)*format)) {
                format++;
            }
            continue;
        }

        if (*format != '%') {
            if (*in != *format) {
                return NULL;
            }
            in++;
            format++;
            continue;
        }

        format++;
        specifier = *format;
        if (specifier == '\0') {
            errno = EINVAL;
            return NULL;
        }
        format++;

        switch (specifier) {
        case '%':
            if (*in != '%') {
                return NULL;
            }
            in++;
            break;
        case 'n':
        case 't':
            skip_space(&in);
            break;
        case 'a':
        case 'A':
            if (name_read(&in, g_day_short_s, g_day_long_s, 7, &parts->tm_wday) != 0) {
                return NULL;
            }
            break;
        case 'b':
        case 'B':
        case 'h':
            if (name_read(&in, g_month_short_s, g_month_long_s, 12, &parts->tm_mon) != 0) {
                return NULL;
            }
            break;
        case 'c':
            rest = strptime(in, "%a %b %e %H:%M:%S %Y", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'C':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 0, 99)) {
                return NULL;
            }
            century = value;
            parts->tm_year = value * 100 - 1900;
            break;
        case 'd':
        case 'e':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 1, 31)) {
                return NULL;
            }
            parts->tm_mday = value;
            break;
        case 'D':
            rest = strptime(in, "%m/%d/%y", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'F':
            rest = strptime(in, "%Y-%m-%d", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'H':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 0, 23)) {
                return NULL;
            }
            parts->tm_hour = value;
            hour12 = -1;
            break;
        case 'I':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 1, 12)) {
                return NULL;
            }
            hour12 = value;
            break;
        case 'j':
            if (number_read(&in, 3, &value) != 0 || !in_range(value, 1, 366)) {
                return NULL;
            }
            parts->tm_yday = value - 1;
            break;
        case 'm':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 1, 12)) {
                return NULL;
            }
            parts->tm_mon = value - 1;
            break;
        case 'M':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 0, 59)) {
                return NULL;
            }
            parts->tm_min = value;
            break;
        case 'p':
            if (word_prefix(in, "AM")) {
                in += 2;
                afternoon = 0;
            } else if (word_prefix(in, "PM")) {
                in += 2;
                afternoon = 1;
            } else {
                return NULL;
            }
            break;
        case 'r':
            rest = strptime(in, "%I:%M:%S %p", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'R':
            rest = strptime(in, "%H:%M", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'S':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 0, 60)) {
                return NULL;
            }
            parts->tm_sec = value;
            break;
        case 'T':
        case 'X':
            rest = strptime(in, "%H:%M:%S", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'u':
            if (number_read(&in, 1, &value) != 0 || !in_range(value, 1, 7)) {
                return NULL;
            }
            parts->tm_wday = value % 7;
            break;
        case 'w':
            if (number_read(&in, 1, &value) != 0 || !in_range(value, 0, 6)) {
                return NULL;
            }
            parts->tm_wday = value;
            break;
        case 'x':
            rest = strptime(in, "%m/%d/%y", parts);
            if (rest == NULL) {
                return NULL;
            }
            in = rest;
            break;
        case 'y':
            if (number_read(&in, 2, &value) != 0 || !in_range(value, 0, 99)) {
                return NULL;
            }
            if (century >= 0) {
                parts->tm_year = century * 100 + value - 1900;
            } else {
                parts->tm_year = (value < 69) ? value + 100 : value;
            }
            break;
        case 'Y':
            if (number_read(&in, 4, &value) != 0) {
                return NULL;
            }
            parts->tm_year = value - 1900;
            break;
        case 'z':
            if (zone_read(&in) != 0) {
                return NULL;
            }
            break;
        case 'Z':
            if (zone_name_read(&in) != 0) {
                return NULL;
            }
            break;
        default:
            errno = EINVAL;
            return NULL;
        }
    }

    if (hour12 >= 0) {
        int hour = hour12 % 12;

        if (afternoon == 1) {
            hour += 12;
        }
        parts->tm_hour = hour;
    }

    return (char *)in;
}
