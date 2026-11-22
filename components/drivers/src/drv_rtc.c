/*
 * Micro OS — RTC driver (BM8563) of the StickC Plus2.
 *
 * The clock is on the I2C bus at address 0x51. The check reads the
 * seven registers of the time in one transaction and lets the core of
 * rtc_time.c decide whether they can be believed: the LV flag of the
 * chip and the plain range of every field. A board whose backup
 * battery went away keeps no time between the power cycles, and the
 * doctor says so out loud — with the date and the time it found —
 * instead of showing a plausible lie.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"
#include "drivers/rtc.h"
#include "drivers/rtc_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/clock.h"
#include "kernel/kmod.h"
#include "kshell/kshell.h"

/*
 * The registers of the chip: two of control and status, and the time
 * after them. The map is the one of the PCF8563 family, which the
 * BM8563 belongs to — reading the time from 0x00 (as the code did
 * once) reads the control registers instead, and the seconds of the
 * clock come out as "hours" like 26:00:00.
 */
#define BM_REG_CTRL1 0x00 /* bit 5 stops the oscillator */
#define BM_REG_CTRL2 0x01 /* the flags of the alarm and the timer, and their enables */
#define BM_REG_SEC 0x02   /* seconds, bit 7 = LV */
#define BM_REG_MIN 0x03
#define BM_REG_HOUR 0x04
#define BM_REG_DAY 0x05
#define BM_REG_WEEKDAY 0x06
#define BM_REG_MONTH 0x07 /* bit 7 = century: 1 = 19xx */
#define BM_REG_YEAR 0x08

/* The alarm follows the time: a minute, an hour, a day and a weekday. */
#define BM_REG_ALARM_MIN 0x09
#define BM_REG_ALARM_HOUR 0x0A
#define BM_REG_ALARM_DAY 0x0B
#define BM_REG_ALARM_WEEKDAY 0x0C

/* Bit 7 of an alarm register: this field is not compared ("alarm enable"). */
#define BM_ALARM_AE 0x80

/*
 * The bits of CTRL2. The BM8563 orders them its own way and not the way the
 * PCF8563 does: here the timers sit in the low bits and the alarm above
 * them. The flag of an alarm is bit 3, and the interrupt of an alarm is
 * enabled by bit 1 — a write of 0x01 (the AIE of the PCF8563) only arms the
 * timer and leaves the INT line high, which is exactly what the first
 * version of this driver did.
 */
#define BM_CTRL2_TIE 0x01 /* the timer interrupt is enabled */
#define BM_CTRL2_AIE 0x02 /* the alarm interrupt is enabled */
#define BM_CTRL2_TF 0x04  /* the timer went off, cleared by writing 0 */
#define BM_CTRL2_AF 0x08  /* the alarm went off, cleared by writing 0 */

/* The flag mask of an alarm register: bits 6..0 hold the BCD value. */
#define BM_ALARM_VALUE 0x7F

/* How far a "+seconds" alarm may look ahead, in the seconds of a day. */
#define RTC_ALARM_MAX_AHEAD (31UL * 24UL * 60UL * 60UL)

/* How far the search for the next match of an alarm may walk. A month is
 * enough for every alarm the chip can express except a day that the next 31
 * days do not have - the 31st asked on the 1st of February. */
#define RTC_ALARM_LOOKAHEAD_DAYS 31

static bool s_present;
static struct rtc_datetime s_now;

/* The alarm flag found while the chip came up: what the last boot must say. */
static bool s_alarm_at_boot;

/* Whether the chip was still comparing an alarm at the boot: an alarm is a
 * property of the clock and survives a restart, so the boot does not wipe
 * it. */
static bool s_alarm_armed_at_boot;

/* The moment as one line, with the verdict of the doctor behind it. */
static const char *rtc_line(char *buf, size_t size)
{
    char when[24];

    rtc_format(&s_now, when, sizeof(when));
    snprintf(buf, size, "%s%s", when, s_now.valid ? "" : " (no clock)");

    return buf;
}

static int drv_rtc_init(void)
{
    uint8_t probe = 0;
    uint8_t status = 0;
    uint8_t control[2] = {0x00, 0x00};

    if (drv_i2c_init() != 0) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, &probe, 1) != 0) {
        return -2;
    }

    /*
     * The flag of the alarm is read before the control registers are put
     * back to their plain state: it is the only record of an alarm that
     * woke the board, and the write below is about to clear it.
     */
    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_CTRL2, &status, 1) == 0) {
        s_alarm_at_boot = (status & BM_CTRL2_AF) != 0;
        s_alarm_armed_at_boot = (status & BM_CTRL2_AIE) != 0;
    }

    /*
     * The control registers are put into their plain state, except the one
     * bit that says "compare the alarm": the alarm belongs to the clock and
     * outlives the firmware (a restart, a formatted volume, a factory reset,
     * a battery that keeps the clock), so wiping it would quietly take away
     * the only wake source of a board that is off. The two flags are cleared
     * - the INT line goes high again - and the oscillator keeps running.
     */
    control[1] = s_alarm_armed_at_boot ? BM_CTRL2_AIE : 0x00;

    /*
     * A failure to write them is not fatal — the clock reads the time either
     * way — so it is not counted against the module.
     */
    (void)drv_i2c_write(I2C_BM8563_ADDR, BM_REG_CTRL1, control, sizeof(control));

    s_present = true;
    return 0;
}

/* Read the running time into s_now. Returns 0 when the chip answered. */
static int rtc_read_now(void)
{
    uint8_t raw[7];

    if (!s_present) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) != 0) {
        return -2;
    }

    rtc_datetime_decode(raw, &s_now);

    return 0;
}

static int drv_rtc_check(void)
{
    if (rtc_read_now() != 0) {
        return -1;
    }

    return s_now.valid ? 0 : 1;
}

static const char *drv_rtc_detail(void)
{
    static char buf[48];
    struct drv_rtc_alarm alarm;
    bool fired = false;
    unsigned hour = (unsigned)(s_now.hour % 100);
    unsigned minute = (unsigned)(s_now.minute % 100);
    unsigned second = (unsigned)(s_now.second % 100);

    /*
     * The line carries the alarm when one is armed: it is the only wake
     * source of a board that is off, and the report is where a person looks
     * for it. The fields of a chip with a dead battery are not in range, so
     * they are cut to two digits before they are printed.
     */
    if (drv_rtc_alarm_read(&alarm, &fired) == 0 &&
        (alarm.hour >= 0 || alarm.minute >= 0 || alarm.day >= 0)) {
        unsigned alarm_hour = (unsigned)(alarm.hour >= 0 ? alarm.hour % 100 : 0);
        unsigned alarm_minute = (unsigned)(alarm.minute >= 0 ? alarm.minute % 100 : 0);

        if (alarm.day >= 0) {
            snprintf(buf, sizeof(buf), "%02u:%02u:%02u, alarm day %u %02u:%02u%s", hour, minute,
                     second, (unsigned)(alarm.day % 100), alarm_hour, alarm_minute,
                     fired ? " fired" : "");
        } else {
            snprintf(buf, sizeof(buf), "%02u:%02u:%02u, alarm %02u:%02u%s", hour, minute, second,
                     alarm_hour, alarm_minute, fired ? " fired" : "");
        }

        return buf;
    }

    /* The report of the doctor stays one line of time, as it was. */
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u%s", hour, minute, second,
             s_now.valid ? "" : " (no clock)");

    return buf;
}

static const struct kmod s_rtc_module = {
    .name = "rtc",
    .desc = "BM8563 real time clock on I2C",
    .init = drv_rtc_init,
    .check = drv_rtc_check,
    .detail = drv_rtc_detail,
};

/*
 * The shell: `date` shows the moment the chip holds, and
 * `date set <YYYY-MM-DD> <HH:MM:SS>` writes a new one. The write goes
 * into the seven registers in one transaction; what is printed after
 * it is read back from the chip, so the line is what the clock really
 * holds and not what was asked of it.
 */
/*
 * Write a moment into the chip: the seven registers in one transaction, and
 * the read back so that `s_now` is what the clock really holds.
 */
int drv_rtc_set_time(const struct rtc_datetime *dt)
{
    uint8_t raw[7];

    if (dt == NULL || !s_present) {
        return -1;
    }

    if (!rtc_datetime_in_range(dt)) {
        return -2;
    }

    rtc_datetime_encode(dt, raw);

    if (drv_i2c_write(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) != 0) {
        return -3;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) == 0) {
        rtc_datetime_decode(raw, &s_now);
    }

    return 0;
}

/* --- the alarm --- */

static bool rtc_alarm_in_range(const struct drv_rtc_alarm *alarm)
{
    if (alarm->hour < -1 || alarm->hour > 23) {
        return false;
    }
    if (alarm->minute < -1 || alarm->minute > 59) {
        return false;
    }
    if (alarm->day < -1 || alarm->day > 31) {
        return false;
    }

    /* An alarm without a single field would go off at any moment, which is
     * not a promise anybody can keep. */
    return alarm->hour >= 0 || alarm->minute >= 0 || alarm->day >= 0;
}

int drv_rtc_alarm_set(const struct drv_rtc_alarm *alarm)
{
    uint8_t raw[4];
    uint8_t ctrl2 = BM_CTRL2_AIE;

    if (alarm == NULL || !s_present) {
        return -1;
    }

    if (!rtc_alarm_in_range(alarm)) {
        return -2;
    }

    raw[0] = alarm->minute >= 0 ? (uint8_t)(rtc_bin_to_bcd((uint8_t)alarm->minute) & BM_ALARM_VALUE)
                                : BM_ALARM_AE;
    raw[1] = alarm->hour >= 0 ? (uint8_t)(rtc_bin_to_bcd((uint8_t)alarm->hour) & BM_ALARM_VALUE)
                              : BM_ALARM_AE;
    raw[2] = alarm->day >= 0 ? (uint8_t)(rtc_bin_to_bcd((uint8_t)alarm->day) & BM_ALARM_VALUE)
                             : BM_ALARM_AE;
    /*
     * The weekday is never compared: the driver computes the weekday from
     * the date, and after a dead battery the register of the chip means
     * nothing.
     */
    raw[3] = BM_ALARM_AE;

    if (drv_i2c_write(I2C_BM8563_ADDR, BM_REG_ALARM_MIN, raw, sizeof(raw)) != 0) {
        return -3;
    }

    /*
     * The enable goes last: the chip must not compare a half written set
     * of registers, and writing CTRL2 also clears a flag of an older alarm.
     */
    if (drv_i2c_write(I2C_BM8563_ADDR, BM_REG_CTRL2, &ctrl2, 1) != 0) {
        return -4;
    }

    return 0;
}

int drv_rtc_alarm_read(struct drv_rtc_alarm *alarm, bool *fired)
{
    uint8_t raw[4];
    uint8_t ctrl2 = 0;

    if (alarm == NULL || !s_present) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_ALARM_MIN, raw, sizeof(raw)) != 0) {
        return -1;
    }
    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_CTRL2, &ctrl2, 1) != 0) {
        return -1;
    }

    if (fired != NULL) {
        *fired = (ctrl2 & BM_CTRL2_AF) != 0;
    }

    /*
     * The registers keep the values of the last alarm even when the chip no
     * longer compares them, so the enable bit decides whether there is an
     * alarm at all.
     */
    if ((ctrl2 & BM_CTRL2_AIE) == 0) {
        alarm->hour = -1;
        alarm->minute = -1;
        alarm->day = -1;
        return 0;
    }

    alarm->minute = (raw[0] & BM_ALARM_AE) ? -1 : (int)rtc_bcd_to_bin(raw[0] & BM_ALARM_VALUE);
    alarm->hour = (raw[1] & BM_ALARM_AE) ? -1 : (int)rtc_bcd_to_bin(raw[1] & BM_ALARM_VALUE);
    alarm->day = (raw[2] & BM_ALARM_AE) ? -1 : (int)rtc_bcd_to_bin(raw[2] & BM_ALARM_VALUE);

    return 0;
}

int drv_rtc_alarm_clear(void)
{
    uint8_t ctrl2 = 0;

    if (!s_present) {
        return -1;
    }

    /* AIE and AF are separate bits of one register: writing zero disables
     * the comparison and releases the INT line in the same transaction. */
    if (drv_i2c_write(I2C_BM8563_ADDR, BM_REG_CTRL2, &ctrl2, 1) != 0) {
        return -1;
    }

    return 0;
}

bool drv_rtc_alarm_was_fired(void)
{
    return s_alarm_at_boot;
}

bool drv_rtc_alarm_was_armed(void)
{
    return s_alarm_armed_at_boot;
}

int drv_rtc_alarm_seconds_ahead(k_u32_t *seconds)
{
    struct drv_rtc_alarm alarm;
    bool fired = false;
    uint32_t ahead = 0;

    if (seconds == NULL) {
        return -1;
    }

    if (drv_rtc_alarm_read(&alarm, &fired) != 0) {
        return -1;
    }

    if (alarm.hour < 0 && alarm.minute < 0 && alarm.day < 0) {
        return -2;
    }

    if (rtc_read_now() != 0 || !s_now.valid) {
        return -3;
    }

    if (!rtc_alarm_seconds_ahead(&s_now, &alarm, RTC_ALARM_LOOKAHEAD_DAYS, &ahead)) {
        return -4;
    }

    *seconds = (k_u32_t)ahead;

    return 0;
}

/*
 * The next whole minute at or after `now + seconds`: the chip compares a
 * minute, so an alarm that answers "+30" goes off at the start of the next
 * minute and not in thirty seconds. The day is filled in when the moment
 * moves to another day, so a short alarm does not become a daily one.
 */
static bool rtc_alarm_from_seconds(const struct rtc_datetime *now, unsigned long seconds,
                                   struct drv_rtc_alarm *alarm)
{
    unsigned long minutes = (seconds + 59UL) / 60UL;
    int days = (int)(minutes / (24UL * 60UL));
    int rest = (int)(minutes % (24UL * 60UL));
    int year = now->year;
    int month = now->month;
    int day = now->day;
    int hour = now->hour + rest / 60;
    int minute = now->minute + rest % 60;

    if (minute >= 60) {
        minute -= 60;
        hour++;
    }
    if (hour >= 24) {
        hour -= 24;
        days++;
    }

    while (days-- > 0) {
        day++;
        if (day > rtc_days_in_month(year, month)) {
            day = 1;
            if (++month > 12) {
                month = 1;
                if (++year > 2099) {
                    return false;
                }
            }
        }
    }

    alarm->hour = hour;
    alarm->minute = minute;
    alarm->day = day;

    return true;
}

/* "12:34" as the hour and the minute of an alarm; no day of the month. */
static bool rtc_parse_alarm_time(const char *s, struct drv_rtc_alarm *alarm)
{
    struct rtc_datetime dt;
    char full[9];

    if (s == NULL) {
        return false;
    }

    /*
     * The parser of the clock speaks HH:MM:SS, and an alarm is a minute:
     * a line without seconds gets ":00" behind it, an hour with seconds is
     * read as it is.
     */
    if (strlen(s) == 5) {
        snprintf(full, sizeof(full), "%s:00", s);
        s = full;
    }

    memset(&dt, 0, sizeof(dt));

    if (!rtc_parse_time(s, &dt)) {
        return false;
    }

    /* The parser of the clock checks the shape, not the range of a field. */
    if (dt.hour > 23 || dt.minute > 59) {
        return false;
    }

    alarm->hour = dt.hour;
    alarm->minute = dt.minute;
    alarm->day = -1;

    return true;
}

/* "+30" as the seconds from now; the whole number and nothing else. */
static bool rtc_parse_alarm_ahead(const char *s, unsigned long *seconds)
{
    char *end = NULL;
    unsigned long value;

    if (s[0] != '+' || s[1] == '\0') {
        return false;
    }

    value = strtoul(s + 1, &end, 10);
    if (end == NULL || *end != '\0' || value == 0 || value > RTC_ALARM_MAX_AHEAD) {
        return false;
    }

    *seconds = value;
    return true;
}

/* The alarm as one line for the shell: what it compares and whether it went off. */
static const char *rtc_alarm_line(const struct drv_rtc_alarm *alarm, bool fired)
{
    static char buf[48];

    if (alarm->hour < 0 && alarm->minute < 0 && alarm->day < 0) {
        snprintf(buf, sizeof(buf), "off");
        return buf;
    }

    if (alarm->day >= 0) {
        snprintf(buf, sizeof(buf), "day %d %02d:%02d, %s", alarm->day,
                 alarm->hour >= 0 ? alarm->hour : 0, alarm->minute >= 0 ? alarm->minute : 0,
                 fired ? "fired" : "not fired");
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d, %s", alarm->hour >= 0 ? alarm->hour : 0,
                 alarm->minute >= 0 ? alarm->minute : 0, fired ? "fired" : "not fired");
    }

    return buf;
}

static void rtc_alarm_usage(void)
{
    kshell_printf("usage: rtc alarm | rtc alarm <HH:MM> | rtc alarm +<seconds>\n");
    kshell_printf("       rtc alarm day <1-31> <HH:MM> | rtc alarm clear\n");
}

/* The command "rtc alarm": set, show or clear the alarm of the chip. */
static int cmd_rtc(int argc, char **argv)
{
    struct drv_rtc_alarm alarm;
    bool fired = false;

    /*
     * "rtc" is the command and "alarm" is its only subcommand today: the
     * word is dropped here, so the rest of the function reads as if the
     * alarm were a command of its own.
     */
    if (argc < 2 || strcmp(argv[1], "alarm") != 0) {
        rtc_alarm_usage();
        return KSHELL_USAGE;
    }

    argc--;
    argv++;

    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        if (drv_rtc_alarm_clear() != 0) {
            kshell_printf("rtc alarm: the clock did not take the clear\n");
            return KSHELL_ERROR;
        }

        kshell_printf("rtc alarm: off\n");
        return KSHELL_OK;
    }

    if (argc == 2) {
        unsigned long seconds;

        memset(&alarm, 0, sizeof(alarm));
        alarm.hour = -1;
        alarm.minute = -1;
        alarm.day = -1;

        if (rtc_parse_alarm_ahead(argv[1], &seconds)) {
            if (!s_present) {
                kshell_printf("rtc alarm: there is no clock to set\n");
                return KSHELL_ERROR;
            }

            if (rtc_read_now() != 0) {
                kshell_printf("rtc alarm: the clock does not answer\n");
                return KSHELL_ERROR;
            }
            if (!s_now.valid) {
                kshell_printf("rtc alarm: the clock has no time to count from\n");
                kshell_printf("rtc alarm: set it first: date set <YYYY-MM-DD> <HH:MM:SS>\n");
                return KSHELL_ERROR;
            }

            if (!rtc_alarm_from_seconds(&s_now, seconds, &alarm)) {
                kshell_printf("rtc alarm: %s is too far ahead\n", argv[1]);
                return KSHELL_ERROR;
            }
        } else if (!rtc_parse_alarm_time(argv[1], &alarm)) {
            if (argv[1][0] == '+') {
                kshell_printf("rtc alarm: no such number of seconds: %s\n", argv[1]);
                kshell_printf("rtc alarm: 1..%lu seconds\n", (unsigned long)RTC_ALARM_MAX_AHEAD);
            } else {
                kshell_printf("rtc alarm: no such time: %s\n", argv[1]);
            }
            rtc_alarm_usage();
            return KSHELL_ERROR;
        }
    } else if (argc == 4 && strcmp(argv[1], "day") == 0) {
        char *end = NULL;
        long day = strtol(argv[2], &end, 10);

        if (end == NULL || *end != '\0' || day < 1 || day > 31) {
            kshell_printf("rtc alarm: no such day of the month: %s\n", argv[2]);
            return KSHELL_ERROR;
        }

        if (!rtc_parse_alarm_time(argv[3], &alarm)) {
            kshell_printf("rtc alarm: no such time: %s\n", argv[3]);
            rtc_alarm_usage();
            return KSHELL_ERROR;
        }

        alarm.day = (int)day;
    } else if (argc == 1) {
        if (drv_rtc_alarm_read(&alarm, &fired) != 0) {
            kshell_printf("rtc alarm: the clock does not answer\n");
            return KSHELL_ERROR;
        }

        kshell_printf("rtc alarm: %s\n", rtc_alarm_line(&alarm, fired));
        return KSHELL_OK;
    } else {
        rtc_alarm_usage();
        return KSHELL_USAGE;
    }

    switch (drv_rtc_alarm_set(&alarm)) {
    case 0:
        break;
    case -1:
        kshell_printf("rtc alarm: there is no clock to set\n");
        return KSHELL_ERROR;
    case -2:
        kshell_printf("rtc alarm: no such time for the alarm\n");
        return KSHELL_ERROR;
    case -3:
        kshell_printf("rtc alarm: the clock did not take the alarm\n");
        return KSHELL_ERROR;
    default:
        kshell_printf("rtc alarm: the clock did not enable the alarm\n");
        return KSHELL_ERROR;
    }

    /* Read back, so the line is what the chip holds and not what was asked. */
    memset(&alarm, 0, sizeof(alarm));
    if (drv_rtc_alarm_read(&alarm, &fired) != 0) {
        kshell_printf("rtc alarm: set, but the clock does not answer\n");
        return KSHELL_OK;
    }

    kshell_printf("rtc alarm: %s\n", rtc_alarm_line(&alarm, fired));

    return KSHELL_OK;
}

static int cmd_date(int argc, char **argv)
{
    char buf[48];

    if (argc == 1) {
        /* The line is read from the chip now, not at the boot. */
        if (rtc_read_now() != 0) {
            kshell_printf("date: the clock does not answer\n");
            return KSHELL_ERROR;
        }

        kshell_printf("%s\n", rtc_line(buf, sizeof(buf)));
        return KSHELL_OK;
    }

    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        struct rtc_datetime dt;

        memset(&dt, 0, sizeof(dt));

        if (!rtc_parse_date(argv[2], &dt) || !rtc_parse_time(argv[3], &dt)) {
            kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
            return KSHELL_USAGE;
        }

        if (!rtc_datetime_in_range(&dt)) {
            kshell_printf("date set: no such day: %s %s\n", argv[2], argv[3]);
            return KSHELL_ERROR;
        }

        if (!s_present) {
            kshell_printf("date set: there is no clock to set\n");
            return KSHELL_ERROR;
        }

        if (drv_rtc_set_time(&dt) != 0) {
            kshell_printf("date set: the clock did not take the time\n");
            return KSHELL_ERROR;
        }

        kshell_printf("%s\n", rtc_line(buf, sizeof(buf)));
        return KSHELL_OK;
    }

    kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
    return KSHELL_USAGE;
}

/*
 * The clock of the system, as the driver of the chip sees it: the kernel
 * asks for the seconds of Unix, the driver turns them into the moment of
 * the BM8563. A service like NTP sets the time through the kernel and does
 * not need to know which chip keeps it - nor to format a command for the
 * shell.
 */
static int rtc_clock_set(k_u64_t unix_seconds)
{
    struct rtc_datetime dt;

    if (!rtc_from_unix(unix_seconds, &dt)) {
        return -1;
    }

    return drv_rtc_set_time(&dt);
}

int drv_rtc_register(void)
{
    int rc = kmod_register(&s_rtc_module);

    kshell_register("date", "the time of the RTC", cmd_date);
    kshell_register("rtc", "the alarm of the RTC: rtc alarm ...", cmd_rtc);
    k_clock_register(rtc_clock_set);

    return rc;
}
