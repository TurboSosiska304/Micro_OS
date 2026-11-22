/*
 * Micro OS — the clock and the calendar of a program.
 *
 * The machine has one clock, `uptime`: the milliseconds since the boot. There
 * is no wall clock (the RTC of the board is not on the x86-64 machine, and the
 * kernel has no call for a date), so `time()` answers the seconds since the
 * boot. The filesystem keeps a moment of Unix in every record, and a listing
 * prints that moment; a record of the disk image of this tree carries zero,
 * which is the first of January 1970, and the line it prints is the same on
 * every boot - a property the smoke test relies on.
 *
 * `gmtime` and `localtime` are the same function: there is no zone and no
 * daylight saving on a machine whose clock starts at the boot. `strftime`
 * knows the conversions a listing and a `find` use, and writes nothing that
 * does not fit the buffer of the caller.
 */
#include <time.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static struct tm g_tm_s;

/* The days since the epoch to a calendar date, the algorithm of civil_from_days. */
static void time_break(time_t when, struct tm *out)
{
    long long seconds = (long long)when;
    long long days = seconds / 86400;
    long long rest = seconds % 86400;
    long long z;
    long long era;
    unsigned long long doe;
    unsigned long long yoe;
    long long year;
    unsigned long long doy;
    unsigned long long mp;

    if (rest < 0) {
        rest += 86400;
        days--;
    }

    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long long)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = (long long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;

    out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->tm_mon = (int)(mp < 10 ? mp + 2 : mp - 10);
    out->tm_year = (int)(year + (out->tm_mon <= 1 ? 1 : 0) - 1900);
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)((rest % 3600) / 60);
    out->tm_sec = (int)(rest % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }
    out->tm_yday = 0;
    out->tm_isdst = 0;
}

int clock_gettime(int clock, struct timespec *moment)
{
    long milliseconds = uptime();

    (void)clock;

    if (moment == NULL) {
        errno = EINVAL;
        return -1;
    }

    moment->tv_sec = (time_t)(milliseconds / 1000);
    moment->tv_nsec = (long)((milliseconds % 1000) * 1000000L);

    return 0;
}

time_t time(time_t *out)
{
    long milliseconds = uptime();
    time_t seconds = (time_t)(milliseconds / 1000);

    if (out != NULL) {
        *out = seconds;
    }

    return seconds;
}

clock_t clock(void)
{
    return (clock_t)uptime() * 1000;
}

struct tm *gmtime(const time_t *when)
{
    if (when == NULL) {
        return NULL;
    }

    time_break(*when, &g_tm_s);

    return &g_tm_s;
}

struct tm *localtime(const time_t *when)
{
    return gmtime(when);
}

static void put_number(char *buffer, size_t size, size_t *used, long value, int width, char fill)
{
    char digits[24];
    int count = 0;
    unsigned long magnitude =
        (value < 0) ? (unsigned long)0 - (unsigned long)value : (unsigned long)value;

    if (value < 0) {
        digits[count++] = '-';
    }
    if (magnitude == 0) {
        digits[count++] = '0';
    }
    while (magnitude > 0) {
        digits[count++] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    }

    while (count < width) {
        if (*used + 1 < size) {
            buffer[(*used)++] = fill;
        }
        width--;
    }

    while (count > 0) {
        if (*used + 1 < size) {
            buffer[(*used)++] = digits[--count];
        } else {
            count--;
        }
    }
}

static void put_text(char *buffer, size_t size, size_t *used, const char *text)
{
    while (*text != '\0') {
        if (*used + 1 < size) {
            buffer[(*used)++] = *text;
        }
        text++;
    }
}

size_t strftime(char *buffer, size_t size, const char *format, const struct tm *parts)
{
    static const char *const days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January",   "February", "March",    "April",
                                         "May",       "June",     "July",     "August",
                                         "September", "October",  "November", "December"};
    size_t used = 0;
    int wrote = 0;

    if (buffer == NULL || size == 0 || format == NULL || parts == NULL) {
        return 0;
    }
    buffer[0] = '\0';

    while (*format != '\0') {
        if (*format != '%') {
            if (used + 1 < size) {
                buffer[used++] = *format;
                wrote = 1;
            }
            format++;
            continue;
        }

        format++;
        wrote = 1;
        switch (*format) {
        case 'a': {
            const char *day = days[parts->tm_wday % 7];

            for (int i = 0; i < 3; i++) {
                if (used + 1 < size) {
                    buffer[used++] = day[i];
                }
            }
            break;
        }
        case 'A':
            put_text(buffer, size, &used, days[parts->tm_wday % 7]);
            break;
        case 'b':
        case 'h': {
            const char *month = months[parts->tm_mon % 12];

            if (used + 3 < size) {
                buffer[used++] = month[0];
                buffer[used++] = month[1];
                buffer[used++] = month[2];
            }
            break;
        }
        case 'B':
            put_text(buffer, size, &used, months[parts->tm_mon % 12]);
            break;
        case 'd':
            put_number(buffer, size, &used, parts->tm_mday, 2, '0');
            break;
        case 'e':
            put_number(buffer, size, &used, parts->tm_mday, 2, ' ');
            break;
        case 'H':
            put_number(buffer, size, &used, parts->tm_hour, 2, '0');
            break;
        case 'M':
            put_number(buffer, size, &used, parts->tm_min, 2, '0');
            break;
        case 'S':
            put_number(buffer, size, &used, parts->tm_sec, 2, '0');
            break;
        case 'Y':
            put_number(buffer, size, &used, (long)parts->tm_year + 1900, 4, '0');
            break;
        case 'y':
            put_number(buffer, size, &used, ((long)parts->tm_year + 1900) % 100, 2, '0');
            break;
        case 'm':
            put_number(buffer, size, &used, parts->tm_mon + 1, 2, '0');
            break;
        case 'j':
            put_number(buffer, size, &used, parts->tm_yday + 1, 3, '0');
            break;
        case 'n':
            put_text(buffer, size, &used, "\n");
            break;
        case 't':
            put_text(buffer, size, &used, "\t");
            break;
        case '%':
            if (used + 1 < size) {
                buffer[used++] = '%';
            }
            break;
        default:
            if (used + 1 < size) {
                buffer[used++] = '%';
            }
            if (used + 1 < size) {
                buffer[used++] = *format;
            }
            break;
        }

        if (*format != '\0') {
            format++;
        }
    }

    buffer[used < size ? used : size - 1] = '\0';

    return wrote ? used : 0;
}

/*
 * The fixed form of a moment: "Www Mmm dd hh:mm:ss yyyy\n", the 26 bytes of
 * `asctime` and the one string a caller can keep until its next call. The
 * machine has one clock (the boot), so `ctime` is the same text for the same
 * seconds on every run.
 */
static char g_asctime_s[26];

char *asctime(const struct tm *parts)
{
    if (parts == NULL) {
        return NULL;
    }
    if (strftime(g_asctime_s, sizeof(g_asctime_s), "%a %b %e %H:%M:%S %Y\n", parts) == 0) {
        return NULL;
    }
    return g_asctime_s;
}

char *ctime(const time_t *when)
{
    if (when == NULL) {
        return NULL;
    }
    return asctime(localtime(when));
}

time_t mktime(struct tm *parts)
{
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    long long days = 0;
    int year;
    int month;

    if (parts == NULL) {
        return (time_t)-1;
    }

    year = parts->tm_year + 1900;
    for (int y = 1970; y < year; y++) {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }
    for (month = 0; month < parts->tm_mon && month < 12; month++) {
        days += month_days[month];
        if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
            days++;
        }
    }
    days += parts->tm_mday - 1;

    return (time_t)(days * 86400 + parts->tm_hour * 3600 + parts->tm_min * 60 + parts->tm_sec);
}
