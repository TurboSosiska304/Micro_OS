/*
 * Micro OS — the time of the RTC, as arithmetic.
 *
 * See rtc_time.h for why this file exists on its own. The register
 * order is the one of the BM8563: seconds (with the LV flag), minutes,
 * hours, days, weekdays, months (with the century bit) and years.
 */
#include "drivers/rtc_time.h"

#include <stdio.h>
#include <string.h>

#define RTC_REG_COUNT 7

/* The LV flag of the seconds register: 1 = the voltage was low. */
#define RTC_LV 0x80

/* The century bit of the months register: 1 = 19xx, 0 = 20xx. */
#define RTC_CENTURY 0x80

uint8_t rtc_bcd_to_bin(uint8_t value)
{
    return (uint8_t)((value >> 4) * 10 + (value & 0x0F));
}

uint8_t rtc_bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

int rtc_days_in_month(int year, int month)
{
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12) {
        return 0;
    }

    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);

        return leap ? 29 : 28;
    }

    return days[month];
}

int rtc_weekday(int year, int month, int day)
{
    /* Sakamoto: the days that move the answer, counted from Sunday. */
    static const int shift[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

    if (month < 1 || month > 12) {
        return -1;
    }

    if (month < 3) {
        year -= 1;
    }

    return (year + year / 4 - year / 100 + year / 400 + shift[month - 1] + day) % 7;
}

bool rtc_datetime_in_range(const struct rtc_datetime *dt)
{
    int days;

    if (dt->year < 1900 || dt->year > 2099) {
        return false;
    }

    if (dt->hour > 23 || dt->minute > 59 || dt->second > 59) {
        return false;
    }

    days = rtc_days_in_month(dt->year, dt->month);
    if (days == 0 || dt->day < 1 || dt->day > days) {
        return false;
    }

    return true;
}

void rtc_datetime_decode(const uint8_t raw[7], struct rtc_datetime *out)
{
    int year;

    memset(out, 0, sizeof(*out));

    out->low_voltage = (raw[0] & RTC_LV) != 0;
    out->second = rtc_bcd_to_bin(raw[0] & 0x7F);
    out->minute = rtc_bcd_to_bin(raw[1] & 0x7F);
    out->hour = rtc_bcd_to_bin(raw[2] & 0x7F);
    out->day = rtc_bcd_to_bin(raw[3] & 0x3F);
    out->month = rtc_bcd_to_bin(raw[5] & 0x1F);

    year = rtc_bcd_to_bin(raw[6]);
    out->year = ((raw[5] & RTC_CENTURY) != 0 ? 1900 : 2000) + year;

    /*
     * The weekday of the chip is not read: it only feeds the weekday
     * alarm, and after a dead battery it is random like everything
     * else. The date knows its own day of the week.
     */
    out->weekday = rtc_weekday(out->year, out->month, out->day);

    out->valid = !out->low_voltage && rtc_datetime_in_range(out);
}

void rtc_datetime_encode(const struct rtc_datetime *dt, uint8_t raw[7])
{
    memset(raw, 0, RTC_REG_COUNT);

    raw[0] = rtc_bin_to_bcd((uint8_t)dt->second);
    raw[1] = rtc_bin_to_bcd((uint8_t)dt->minute);
    raw[2] = rtc_bin_to_bcd((uint8_t)dt->hour);
    raw[3] = rtc_bin_to_bcd((uint8_t)dt->day);
    raw[4] = rtc_bin_to_bcd((uint8_t)rtc_weekday(dt->year, dt->month, dt->day));
    raw[5] = rtc_bin_to_bcd((uint8_t)dt->month);
    raw[6] = rtc_bin_to_bcd((uint8_t)(dt->year % 100));

    if (dt->year < 2000) {
        raw[5] |= RTC_CENTURY;
    }
}

/* Exactly four digits, two digits and two digits, nothing else. */
static bool digits(const char *s, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }

    return true;
}

static int number(const char *s)
{
    return (s[0] - '0') * 10 + (s[1] - '0');
}

bool rtc_parse_date(const char *s, struct rtc_datetime *dt)
{
    if (s == NULL || strlen(s) != 10) {
        return false;
    }

    if (!digits(s, 4) || s[4] != '-' || !digits(s + 5, 2) || s[7] != '-' || !digits(s + 8, 2)) {
        return false;
    }

    dt->year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    dt->month = number(s + 5);
    dt->day = number(s + 8);
    dt->weekday = rtc_weekday(dt->year, dt->month, dt->day);

    return true;
}

bool rtc_parse_time(const char *s, struct rtc_datetime *dt)
{
    if (s == NULL || strlen(s) != 8) {
        return false;
    }

    if (!digits(s, 2) || s[2] != ':' || !digits(s + 3, 2) || s[5] != ':' || !digits(s + 6, 2)) {
        return false;
    }

    dt->hour = number(s);
    dt->minute = number(s + 3);
    dt->second = number(s + 6);

    return true;
}

void rtc_format(const struct rtc_datetime *dt, char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }

    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d", dt->year, dt->month, dt->day, dt->hour,
             dt->minute, dt->second);
}
