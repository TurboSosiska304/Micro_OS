/*
 * Micro OS — the time of the RTC, as arithmetic.
 *
 * The BM8563 speaks BCD and packs seven registers: seconds, minutes,
 * hours, days, weekdays, months (with the century bit) and years. Which
 * byte means what, whether the numbers can be believed at all and how
 * a human line of text turns back into registers — that is all pure
 * arithmetic with no hardware in it, which is why the host tests
 * compile the file themselves and push bytes through it.
 *
 * The validity of the time comes from two places: the LV flag of the
 * chip (the voltage was low, the registers are from nobody knows
 * when) and the plain range of every field — a battery that went away
 * leaves hours like 58, and 58 is not an hour.
 */
#ifndef MICRO_OS_DRIVERS_RTC_TIME_H
#define MICRO_OS_DRIVERS_RTC_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One moment of time as the driver sees it. The weekday is not read
 * from the chip: the register of the weekday only feeds the weekday
 * alarm, and after a dead battery it is as random as everything else —
 * so it is computed from the date, which makes the same date always
 * give the same weekday.
 */
struct rtc_datetime {
    int year;         /* 1900..2099, from the century bit and the year byte */
    int month;        /* 1..12 */
    int day;          /* 1..31, as the calendar of that month allows */
    int weekday;      /* 0 = Sunday .. 6 = Saturday */
    int hour;         /* 0..23 */
    int minute;       /* 0..59 */
    int second;       /* 0..59 */
    bool low_voltage; /* the LV flag: the time is not to be trusted */
    bool valid;       /* the flag is clear and every field is in range */
};

/* The two halves of BCD, as the chip speaks it. */
uint8_t rtc_bcd_to_bin(uint8_t value);
uint8_t rtc_bin_to_bcd(uint8_t value);

/* The day of the week of a date: 0 = Sunday .. 6 = Saturday. */
int rtc_weekday(int year, int month, int day);

/* How many days the month has (0 for a month that does not exist). */
int rtc_days_in_month(int year, int month);

/* Every field inside its range (the LV flag plays no part here). */
bool rtc_datetime_in_range(const struct rtc_datetime *dt);

/*
 * The seven registers as the chip sends them — seconds, minutes,
 * hours, days, weekdays, months, years — into a moment of time, with
 * the validity decided from the flag and the ranges.
 */
void rtc_datetime_decode(const uint8_t raw[7], struct rtc_datetime *out);

/*
 * The other way around, for `date set`: the weekday byte is filled in
 * from the date. The LV bit of the seconds register is written as 0 —
 * the flag belongs to the chip, not to the caller.
 */
void rtc_datetime_encode(const struct rtc_datetime *dt, uint8_t raw[7]);

/*
 * The words a person types: "2026-10-26" and "12:34:56", nothing else.
 * The date fills the date of the moment and the time fills the time;
 * false is the answer for anything that is not exactly that shape.
 */
bool rtc_parse_date(const char *s, struct rtc_datetime *dt);
bool rtc_parse_time(const char *s, struct rtc_datetime *dt);

/* The moment as one line: "2026-10-26 12:34:56". */
void rtc_format(const struct rtc_datetime *dt, char *buf, size_t size);

#endif /* MICRO_OS_DRIVERS_RTC_TIME_H */
