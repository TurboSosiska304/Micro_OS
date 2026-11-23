/*
 * Micro OS — the calendar of a program.
 *
 * The seconds come from the clock of the machine, through the call 49
 * (`KSYS_CLOCK_GETTIME`): CLOCK_REALTIME is the wall clock of Unix, which the
 * port seeds at the boot from its own source - the BM8563 of the board, the
 * CMOS of a laptop, the process clock of the host - and which `ntp` or
 * `date set` corrects afterwards. CLOCK_MONOTONIC is the millisecond count of
 * the boot, which is what `uptime` answers. A machine whose clock was never
 * set still answers: the seconds since its boot, which is a number and not a
 * date (docs/debts.md keeps the zone, not the clock).
 *
 * The calendar is the one of UTC (the C locale has no zone): the two functions
 * `localtime` and `gmtime` answer the same structure, and the cost of a real
 * time zone is a debt of `docs/debts.md`.
 *
 * The conversion of a day count to a date is the one of Howard Hinnant's
 * `civil_from_days`: the days are counted from 1970-01-01, the era of 400
 * years absorbs the leap rule, and the day of the year falls out of the same
 * arithmetic. It is integer work only, so it is exact for every second the
 * type can hold.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* The days from 1970-01-01 to 2000-03-01, the start of an era of 400 years. */
#define DAYS_TO_ERA 719468L

static void civil_from_days(long days, int *year, int *month, int *day)
{
    long shifted = days + DAYS_TO_ERA;
    long era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    unsigned long day_of_era = (unsigned long)(shifted - era * 146097);
    unsigned long year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    long year_number = (long)year_of_era + era * 400;
    unsigned long day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    unsigned long month_prime = (5 * day_of_year + 2) / 153;

    *day = (int)(day_of_year - (153 * month_prime + 2) / 5 + 1);
    *month = (int)(month_prime < 10 ? month_prime + 3 : month_prime - 9);
    *year = (int)(year_number + ((*month <= 2) ? 1 : 0));
}

static long days_from_civil(int year, int month, int day)
{
    long year_number = year - ((month <= 2) ? 1 : 0);
    long era = (year_number >= 0 ? year_number : year_number - 399) / 400;
    unsigned long year_of_era = (unsigned long)(year_number - era * 400);
    unsigned long day_of_year =
        (unsigned long)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned long day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    return era * 146097 + (long)day_of_era - DAYS_TO_ERA;
}

static struct tm g_broken_s;

static struct tm *break_down(time_t seconds, struct tm *broken)
{
    long days = (long)(seconds / 86400);
    long rest = (long)(seconds % 86400);
    int year;
    int month;
    int day;
    long day_of_year;

    if (rest < 0) {
        rest += 86400;
        days--;
    }

    civil_from_days(days, &year, &month, &day);

    broken->tm_sec = (int)(rest % 60);
    broken->tm_min = (int)((rest / 60) % 60);
    broken->tm_hour = (int)(rest / 3600);
    broken->tm_mday = day;
    broken->tm_mon = month - 1;
    broken->tm_year = year - 1900;
    /* 1970-01-01 was a Thursday, the fourth day of the week of `tm_wday`. */
    broken->tm_wday = (int)((days + 4) % 7);
    if (broken->tm_wday < 0) {
        broken->tm_wday += 7;
    }
    day_of_year = days_from_civil(year, month, day) - days_from_civil(year, 1, 1);
    broken->tm_yday = (int)day_of_year;
    broken->tm_isdst = 0;

    return broken;
}

/*
 * The moment of one of the two clocks of POSIX. The wall clock is the call 49
 * of the kernel, with the fraction of the second it keeps; a kernel that does
 * not answer it (an image of an older release on a machine of this one) gets
 * the milliseconds of the boot, which is what the call answered before the
 * wall clock existed.
 */
int clock_gettime(int clock, struct timespec *moment)
{
    struct ksys_timespec answer;
    long rc;

    if (moment == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_CLOCK_GETTIME, (long)clock, (long)(uintptr_t)&answer, 0, 0, 0, 0);
    if (rc < 0) {
        long milliseconds = uptime();

        if (milliseconds < 0) {
            errno = (int)-milliseconds;
            return -1;
        }

        moment->tv_sec = (time_t)(milliseconds / 1000);
        moment->tv_nsec = (long)((milliseconds % 1000) * 1000000L);

        return 0;
    }

    /* The wire carries two 64-bit words on both machines; a 32-bit target
     * keeps the seconds in its own `time_t`, which reaches 2038. */
    moment->tv_sec = (time_t)answer.tv_sec;
    moment->tv_nsec = (long)answer.tv_nsec;

    return 0;
}

time_t time(time_t *seconds)
{
    struct timespec moment;
    time_t now;

    if (clock_gettime(CLOCK_REALTIME, &moment) != 0) {
        return (time_t)-1;
    }

    now = moment.tv_sec;

    if (seconds != NULL) {
        *seconds = now;
    }

    return now;
}

/*
 * The processor time of the program, in the milliseconds of CLOCKS_PER_SEC.
 * The kernel counts it for the CPU limit of the task and answers it with the
 * call 50 (`getrusage`), so `clock()` is the time the program spent running
 * and not the time that passed since the boot - which is what `time` and
 * `CLOCK_MONOTONIC` are for.
 */
clock_t clock(void)
{
    struct ksys_rusage usage;
    long rc;

    rc = __ulibc_syscall6(KSYS_GETRUSAGE, (long)(uintptr_t)&usage, 0, 0, 0, 0, 0);
    if (rc < 0) {
        return (clock_t)-1;
    }

    return (clock_t)(usage.utime_ms + usage.stime_ms);
}

#ifdef ULIBC_WITH_FLOAT
double difftime(time_t later, time_t earlier)
{
    return (double)(later - earlier);
}
#endif /* ULIBC_WITH_FLOAT */

struct tm *gmtime(const time_t *seconds)
{
    if (seconds == NULL) {
        return NULL;
    }

    return break_down(*seconds, &g_broken_s);
}

struct tm *localtime(const time_t *seconds)
{
    return gmtime(seconds);
}

time_t mktime(struct tm *broken)
{
    long days;
    time_t seconds;

    if (broken == NULL) {
        return (time_t)-1;
    }

    days = days_from_civil(broken->tm_year + 1900, broken->tm_mon + 1, broken->tm_mday);
    seconds = (time_t)days * 86400 + (time_t)broken->tm_hour * 3600 + (time_t)broken->tm_min * 60 +
              (time_t)broken->tm_sec;
    broken->tm_isdst = 0;

    return seconds;
}

/* --- strftime ----------------------------------------------------------- */

struct outline {
    char *buffer;
    size_t size;
    size_t used;
    int overflow;
};

static void out_byte(struct outline *out, char byte)
{
    if (out->used + 1 < out->size) {
        out->buffer[out->used] = byte;
    } else {
        out->overflow = 1;
    }
    out->used++;
}

static void out_word(struct outline *out, const char *word)
{
    while (*word != '\0') {
        out_byte(out, *word++);
    }
}

static void out_number(struct outline *out, int value, int width)
{
    char digits[8];
    int count = 0;

    if (value < 0) {
        out_byte(out, '-');
        value = -value;
    }

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0 && count < (int)sizeof(digits));

    while (count < width) {
        out_byte(out, '0');
        width--;
    }
    while (count > 0) {
        out_byte(out, digits[--count]);
    }
}

static const char *const g_week_day_short[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const g_week_day_long[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                              "Thursday", "Friday", "Saturday"};
static const char *const g_month_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const g_month_long[] = {"January",   "February", "March",    "April",
                                           "May",       "June",     "July",     "August",
                                           "September", "October",  "November", "December"};

static void format_one(struct outline *out, char specifier, const struct tm *broken)
{
    int wday = (broken->tm_wday >= 0 && broken->tm_wday < 7) ? broken->tm_wday : 0;
    int mon = (broken->tm_mon >= 0 && broken->tm_mon < 12) ? broken->tm_mon : 0;
    int hour12 = broken->tm_hour % 12;

    if (hour12 == 0) {
        hour12 = 12;
    }

    switch (specifier) {
    case 'a':
        out_word(out, g_week_day_short[wday]);
        break;
    case 'A':
        out_word(out, g_week_day_long[wday]);
        break;
    case 'b':
    case 'h':
        out_word(out, g_month_short[mon]);
        break;
    case 'B':
        out_word(out, g_month_long[mon]);
        break;
    case 'c':
        out_word(out, g_week_day_short[wday]);
        out_byte(out, ' ');
        out_word(out, g_month_short[mon]);
        out_byte(out, ' ');
        out_number(out, broken->tm_mday, 2);
        out_byte(out, ' ');
        out_number(out, broken->tm_hour, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_min, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_sec, 2);
        out_byte(out, ' ');
        out_number(out, broken->tm_year + 1900, 4);
        break;
    case 'C':
        out_number(out, (broken->tm_year + 1900) / 100, 2);
        break;
    case 'd':
        out_number(out, broken->tm_mday, 2);
        break;
    case 'D':
        out_number(out, broken->tm_mon + 1, 2);
        out_byte(out, '/');
        out_number(out, broken->tm_mday, 2);
        out_byte(out, '/');
        out_number(out, (broken->tm_year + 1900) % 100, 2);
        break;
    case 'e':
        if (broken->tm_mday < 10) {
            out_byte(out, ' ');
        }
        out_number(out, broken->tm_mday, 1);
        break;
    case 'F':
        out_number(out, broken->tm_year + 1900, 4);
        out_byte(out, '-');
        out_number(out, broken->tm_mon + 1, 2);
        out_byte(out, '-');
        out_number(out, broken->tm_mday, 2);
        break;
    case 'H':
        out_number(out, broken->tm_hour, 2);
        break;
    case 'I':
        out_number(out, hour12, 2);
        break;
    case 'j':
        out_number(out, broken->tm_yday + 1, 3);
        break;
    case 'm':
        out_number(out, broken->tm_mon + 1, 2);
        break;
    case 'M':
        out_number(out, broken->tm_min, 2);
        break;
    case 'n':
        out_byte(out, '\n');
        break;
    case 'p':
        out_word(out, (broken->tm_hour < 12) ? "AM" : "PM");
        break;
    case 'r':
        out_number(out, hour12, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_min, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_sec, 2);
        out_byte(out, ' ');
        out_word(out, (broken->tm_hour < 12) ? "AM" : "PM");
        break;
    case 'R':
        out_number(out, broken->tm_hour, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_min, 2);
        break;
    case 'S':
        out_number(out, broken->tm_sec, 2);
        break;
    case 't':
        out_byte(out, '\t');
        break;
    case 'T':
    case 'X':
        out_number(out, broken->tm_hour, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_min, 2);
        out_byte(out, ':');
        out_number(out, broken->tm_sec, 2);
        break;
    case 'x':
        out_number(out, broken->tm_mon + 1, 2);
        out_byte(out, '/');
        out_number(out, broken->tm_mday, 2);
        out_byte(out, '/');
        out_number(out, (broken->tm_year + 1900) % 100, 2);
        break;
    case 'y':
        out_number(out, (broken->tm_year + 1900) % 100, 2);
        break;
    case 'Y':
        out_number(out, broken->tm_year + 1900, 4);
        break;
    case 'u':
        out_number(out, (wday == 0) ? 7 : wday, 1);
        break;
    case 'w':
        out_number(out, wday, 1);
        break;
    case 'Z':
        out_word(out, "UTC");
        break;
    case 'z':
        out_word(out, "+0000");
        break;
    case '%':
        out_byte(out, '%');
        break;
    default:
        /* A specifier the C locale does not have is left as it was written,
         * which makes a typo of a format visible instead of dropping it. */
        out_byte(out, '%');
        out_byte(out, specifier);
        break;
    }
}

size_t strftime(char *buffer, size_t size, const char *format, const struct tm *broken)
{
    struct outline out;
    const char *cursor;

    if (buffer == NULL || size == 0 || format == NULL || broken == NULL) {
        return 0;
    }

    out.buffer = buffer;
    out.size = size;
    out.used = 0;
    out.overflow = 0;

    for (cursor = format; *cursor != '\0'; cursor++) {
        if (*cursor != '%') {
            out_byte(&out, *cursor);
            continue;
        }

        cursor++;
        if (*cursor == '\0') {
            out_byte(&out, '%');
            break;
        }
        format_one(&out, *cursor, broken);
    }

    buffer[(out.used < size) ? out.used : size - 1] = '\0';

    return out.overflow ? 0 : out.used;
}

/*
 * The fixed form of a moment: "Www Mmm dd hh:mm:ss yyyy\n", the 26 bytes of
 * `asctime` and the one string a caller keeps until the next call. The moment
 * is the one of the wall clock now, so the same second gives the same text on
 * every machine.
 */
static char g_asctime_s[26];

char *asctime(const struct tm *broken)
{
    if (broken == NULL) {
        return NULL;
    }
    if (strftime(g_asctime_s, sizeof(g_asctime_s), "%a %b %e %H:%M:%S %Y\n", broken) == 0) {
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
