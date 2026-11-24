/*
 * Micro OS — `date`, the wall clock of the machine as a command.
 *
 * The time is a number of the kernel (`kernel/clock.h`): the port seeds it
 * from the chip of the machine at the boot - the CMOS of x86-64, the BM8563 of
 * the board, the clock of the host process of the simulator - and a service
 * like NTP sets it through the same call. This command is the face of that
 * number: it prints the seconds of Unix as a line of a person and it sets them
 * back through the kernel, which is why it is a command of the shell and not a
 * program of the disk.
 *
 * A program has `clock_gettime` (call 49) and can print a date; it cannot set
 * one, because setting a clock is a privileged act with no call of the ABI,
 * and a `date` that printed through a call and set through nothing would be
 * two halves of one command in two places. The command is portable on purpose:
 * the board answers it from the kernel's clock and not from the I2C chip of
 * the RTC (the chip is where the number came from at the boot, and reading it
 * for every line is a bus transaction to ask what the kernel already knows),
 * and x86-64 answers it from the CMOS the port read.
 *
 * The format is the one the board has printed since 1.0, `YYYY-MM-DD
 * HH:MM:SS` of UTC, so that a script and a person read the same line whichever
 * port they are on.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"

/* The first and the last year a person may type, and no other: 1970 is the
 * epoch of the seconds and 2099 is the end of the calendar of the chip of the
 * board (a register of two digits and the century of ACPI), so a number
 * outside the two is a typed mistake and not a moment of a machine. */
#define KSHELL_DATE_YEAR_FIRST 1970
#define KSHELL_DATE_YEAR_LAST 2099

/*
 * The days of a civil date, counted from the epoch, by the arithmetic of
 * Howard Hinnant ("chrono-compatible low-level date algorithms"): the months
 * are counted from March, which puts the leap day at the end of a year, and
 * the days are cut into eras of 400 years so that the division is exact for
 * every century. The same function is the inverse of the one below, which is
 * what makes a date of the shell and a date of the RTC agree.
 */
static k_i64_t date_days_from_civil(k_i64_t year, unsigned month, unsigned day)
{
    k_i64_t y = year - (month <= 2u ? 1 : 0);
    k_i64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400); /* 0..399 */
    unsigned doy = (153u * (unsigned)((int)month + ((month > 2u) ? -3 : 9)) + 2u) / 5u + day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy; /* 0..146096 */

    return era * 146097 + (k_i64_t)doe - 719468;
}

/* The date of a number of days since the epoch: the inverse of the above. */
static void date_civil_from_days(k_i64_t days, k_i64_t *year, unsigned *month, unsigned *day)
{
    k_i64_t z = days + 719468;
    k_i64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097); /* 0..146096 */
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    k_i64_t y = (k_i64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); /* 0..365 */
    unsigned mp = (5u * doy + 2u) / 153u;                      /* 0..11, March is 0 */
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = (unsigned)((int)mp + ((mp < 10u) ? 3 : -9));

    *year = y + (m <= 2u ? 1 : 0);
    *month = m;
    *day = d;
}

bool kshell_time_line(k_u64_t unix_seconds, char *buf, k_size_t size)
{
    k_i64_t days;
    k_i64_t year;
    unsigned month;
    unsigned day;
    unsigned second_of_day;

    if (buf == NULL || size == 0) {
        return false;
    }

    days = (k_i64_t)(unix_seconds / 86400u);
    second_of_day = (unsigned)(unix_seconds % 86400u);

    date_civil_from_days(days, &year, &month, &day);

    if (year < KSHELL_DATE_YEAR_FIRST || year > KSHELL_DATE_YEAR_LAST) {
        buf[0] = '\0';
        return false;
    }

    (void)snprintf(buf, (size_t)size, "%04d-%02u-%02u %02u:%02u:%02u", (int)year, month, day,
                   second_of_day / 3600u, (second_of_day / 60u) % 60u, second_of_day % 60u);

    return true;
}

/* A field of `digits` decimal digits, or false: the parser of the input of a
 * person, which refuses `26-11-24` and `2026-1-1` alike - the line `date set`
 * prints is the line it reads. */
static bool date_field(const char *text, k_size_t digits, unsigned *out)
{
    unsigned value = 0;
    k_size_t i;

    for (i = 0; i < digits; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        value = value * 10u + (unsigned)(text[i] - '0');
    }

    *out = value;

    return true;
}

/* `YYYY-MM-DD` and `HH:MM:SS` into the seconds of Unix. */
static bool date_parse(const char *day_text, const char *time_text, k_u64_t *out)
{
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    k_i64_t unix_seconds;

    if (strlen(day_text) != 10u || day_text[4] != '-' || day_text[7] != '-') {
        return false;
    }

    if (!date_field(day_text, 4u, &year) || !date_field(day_text + 5, 2u, &month) ||
        !date_field(day_text + 8, 2u, &day)) {
        return false;
    }

    if (strlen(time_text) != 8u || time_text[2] != ':' || time_text[5] != ':') {
        return false;
    }

    if (!date_field(time_text, 2u, &hour) || !date_field(time_text + 3, 2u, &minute) ||
        !date_field(time_text + 6, 2u, &second)) {
        return false;
    }

    if (year < KSHELL_DATE_YEAR_FIRST || year > KSHELL_DATE_YEAR_LAST) {
        return false;
    }

    if (month < 1u || month > 12u || day < 1u || day > 31u) {
        return false;
    }

    if (hour > 23u || minute > 59u || second > 59u) {
        return false;
    }

    /*
     * A day that the calendar does not have is refused and not carried into
     * the next month: the round trip below is the check, and `2026-02-30`
     * would otherwise set the clock to the 2nd of March without a word.
     */
    {
        k_i64_t days = date_days_from_civil((k_i64_t)year, month, day);
        k_i64_t back_year;
        unsigned back_month;
        unsigned back_day;

        date_civil_from_days(days, &back_year, &back_month, &back_day);

        if (back_year != (k_i64_t)year || back_month != month || back_day != day) {
            return false;
        }

        unix_seconds = days * 86400 + (k_i64_t)hour * 3600 + (k_i64_t)minute * 60 + (k_i64_t)second;
    }

    if (unix_seconds < 0) {
        return false;
    }

    *out = (k_u64_t)unix_seconds;

    return true;
}

/* The line of the moment of the kernel, or the sentence of a machine that has
 * no clock: a number that is only the seconds since the boot is not a date,
 * and printing it as one would be a lie of 1970. */
static int date_print(void)
{
    char line[32];

    if (!k_clock_present() || !kshell_time_line(k_clock_now_unix(), line, sizeof(line))) {
        kshell_printf("date: this machine has no clock\n");
        return KSHELL_ERROR;
    }

    kshell_printf("%s\n", line);

    return KSHELL_OK;
}

static int cmd_date(int argc, char **argv)
{
    k_u64_t when = 0;
    char line[32];

    if (argc == 1) {
        return date_print();
    }

    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        if (!date_parse(argv[2], argv[3], &when)) {
            kshell_printf("date: %s %s is not a moment (YYYY-MM-DD HH:MM:SS)\n", argv[2], argv[3]);
            return KSHELL_ERROR;
        }

        if (!k_clock_present()) {
            kshell_printf("date set: there is no clock to set\n");
            return KSHELL_ERROR;
        }

        if (k_clock_set_unix(when) != 0) {
            kshell_printf("date set: the clock did not take the time\n");
            return KSHELL_ERROR;
        }

        if (!kshell_time_line(k_clock_now_unix(), line, sizeof(line))) {
            kshell_printf("date set: the clock does not answer\n");
            return KSHELL_ERROR;
        }

        kshell_printf("%s\n", line);

        return KSHELL_OK;
    }

    kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");

    return KSHELL_USAGE;
}

int kshell_register_time(void)
{
    static const struct kshell_help date_help = {
        .summary = "the date and the time of the machine (UTC)",
        .usage = "date [set <YYYY-MM-DD> <HH:MM:SS>]",
        .detail = "Without a word the moment of the clock of the kernel, in the seconds of Unix\n"
                  "read as UTC. `date set` writes a new moment through the kernel, which is\n"
                  "where the chip of the machine (the RTC of the board, the CMOS of x86-64)\n"
                  "and the NTP client meet; a machine with no clock says so instead of\n"
                  "printing the seconds since the boot as a date of 1970.\n",
    };

    return kshell_register_help("date", &date_help, cmd_date);
}
