/*
 * Micro OS — the time of the RTC, as arithmetic.
 *
 * The bytes of the BM8563 are checked the way the board really shows
 * them: the flag of a low voltage turns the verdict over, the garbage
 * of a dead battery (hours like 58) never passes for a time, and the
 * calendar knows the days of February — including the century ones.
 * The set path is the round trip: a moment encoded into the seven
 * registers has to come back out of them the same, and the words a
 * person types have to be exactly the words the command asks for.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/rtc_time.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

static void test_bcd(void)
{
    int value;

    ktest_begin("rtc: the BCD of the chip counts both ways");

    for (value = 0; value <= 99; value++) {
        uint8_t encoded = rtc_bin_to_bcd((uint8_t)value);

        ktest_checks++;

        if (rtc_bcd_to_bin(encoded) != (uint8_t)value) {
            ktest_fail(__FILE__, __LINE__, "%d went through BCD as %u", value, encoded);
            break;
        }
    }

    KT_CHECK_INT_EQ(rtc_bin_to_bcd(0), 0x00);
    KT_CHECK_INT_EQ(rtc_bin_to_bcd(9), 0x09);
    KT_CHECK_INT_EQ(rtc_bin_to_bcd(59), 0x59);
    KT_CHECK_INT_EQ(rtc_bin_to_bcd(26), 0x26);
    KT_CHECK_INT_EQ(rtc_bcd_to_bin(0x59), 59);
    KT_CHECK_INT_EQ(rtc_bcd_to_bin(0x23), 23);
}

/* The registers as the chip sends them: sec, min, hour, day, weekday, month, year. */
static void test_decode(void)
{
    static const uint8_t raw[7] = {0x59, 0x34, 0x12, 0x26, 0x03, 0x10, 0x26};
    struct rtc_datetime dt;

    ktest_begin("rtc: the registers of the chip come out as a moment");

    rtc_datetime_decode(raw, &dt);

    KT_CHECK_INT_EQ(dt.year, 2026);
    KT_CHECK_INT_EQ(dt.month, 10);
    KT_CHECK_INT_EQ(dt.day, 26);
    KT_CHECK_INT_EQ(dt.hour, 12);
    KT_CHECK_INT_EQ(dt.minute, 34);
    KT_CHECK_INT_EQ(dt.second, 59);
    /* 2026-10-26 is a Monday, whatever the weekday byte (0x03 here) says. */
    KT_CHECK_INT_EQ(dt.weekday, 1);
    KT_CHECK(!dt.low_voltage);
    KT_CHECK(dt.valid);
}

/*
 * The verdict of H8 of the audit: bit 7 of the seconds register is the
 * LV flag — 1 means the voltage was low and the time is from nobody
 * knows when. Read the other way round, the doctor lied in both
 * directions.
 */
static void test_low_voltage(void)
{
    static const uint8_t good[7] = {0x30, 0x00, 0x12, 0x01, 0x00, 0x01, 0x26};
    static const uint8_t low[7] = {0xB0, 0x00, 0x12, 0x01, 0x00, 0x01, 0x26};
    struct rtc_datetime dt;

    ktest_begin("rtc: a low voltage says the time is not to be trusted");

    rtc_datetime_decode(good, &dt);
    KT_CHECK(!dt.low_voltage);
    KT_CHECK(dt.valid);

    rtc_datetime_decode(low, &dt);
    KT_CHECK(dt.low_voltage);
    KT_CHECK(!dt.valid);
    /* The fields themselves are read all the same: 12:00:30. */
    KT_CHECK_INT_EQ(dt.hour, 12);
    KT_CHECK_INT_EQ(dt.second, 30);
}

/* The battery that went away: the registers keep random values. */
static void test_garbage(void)
{
    struct rtc_datetime dt;

    ktest_begin("rtc: the garbage of a dead battery never passes for a time");

    /* The very state of the board: 58:00:00 (no clock). */
    static const uint8_t hours58[7] = {0x00, 0x00, 0x58, 0x01, 0x00, 0x01, 0x26};

    rtc_datetime_decode(hours58, &dt);
    KT_CHECK_INT_EQ(dt.hour, 58);
    KT_CHECK(!dt.valid);

    static const uint8_t seconds60[7] = {0x60, 0x00, 0x01, 0x01, 0x00, 0x01, 0x26};

    rtc_datetime_decode(seconds60, &dt);
    KT_CHECK_INT_EQ(dt.second, 60);
    KT_CHECK(!dt.valid);

    static const uint8_t month0[7] = {0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x26};

    rtc_datetime_decode(month0, &dt);
    KT_CHECK(!dt.valid);

    static const uint8_t day32[7] = {0x00, 0x00, 0x01, 0x32, 0x00, 0x01, 0x26};

    rtc_datetime_decode(day32, &dt);
    KT_CHECK_INT_EQ(dt.day, 32);
    KT_CHECK(!dt.valid);
}

/* The calendar: February knows its days, including the century ones. */
static void test_calendar(void)
{
    struct rtc_datetime dt;

    ktest_begin("rtc: the calendar knows the days of every month");

    /* 2024 is a leap year: February had 29 days. */
    static const uint8_t leap[7] = {0x00, 0x00, 0x12, 0x29, 0x00, 0x02, 0x24};

    rtc_datetime_decode(leap, &dt);
    KT_CHECK_INT_EQ(dt.year, 2024);
    KT_CHECK(dt.valid);
    /* 2024-02-29 was a Thursday. */
    KT_CHECK_INT_EQ(dt.weekday, 4);

    /* 2026 is not: February had 28 days. */
    static const uint8_t not_leap[7] = {0x00, 0x00, 0x12, 0x29, 0x00, 0x02, 0x26};

    rtc_datetime_decode(not_leap, &dt);
    KT_CHECK(!dt.valid);

    /* 2000 was a leap year (divisible by 400). */
    static const uint8_t y2000[7] = {0x00, 0x00, 0x12, 0x29, 0x00, 0x02, 0x00};

    rtc_datetime_decode(y2000, &dt);
    KT_CHECK_INT_EQ(dt.year, 2000);
    KT_CHECK(dt.valid);

    /* 1900 was not (divisible by 100, not by 400): the century bit matters. */
    static const uint8_t y1900[7] = {0x00, 0x00, 0x12, 0x29, 0x00, 0x82, 0x00};

    rtc_datetime_decode(y1900, &dt);
    KT_CHECK_INT_EQ(dt.year, 1900);
    KT_CHECK(!dt.valid);

    /* April has 30 days, so the 31st of April does not exist. */
    static const uint8_t april31[7] = {0x00, 0x00, 0x12, 0x31, 0x00, 0x04, 0x26};

    rtc_datetime_decode(april31, &dt);
    KT_CHECK(!dt.valid);

    /* The last day of the year exists, of course. */
    static const uint8_t dec31[7] = {0x59, 0x59, 0x23, 0x31, 0x00, 0x0C, 0x26};

    rtc_datetime_decode(dec31, &dt);
    KT_CHECK(dt.valid);
    KT_CHECK_INT_EQ(dt.month, 12);
    KT_CHECK_INT_EQ(dt.day, 31);
}

/* The way back for `date set`: encode, decode, and nothing changed. */
static void test_round_trip(void)
{
    static const struct {
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int second;
    } moments[] = {
        {2026, 10, 26, 12, 34, 59}, /* today, as it was written */
        {2000, 1, 1, 0, 0, 0},      /* the first moment of the century */
        {2099, 12, 31, 23, 59, 59}, /* the last one, on the edge of the range */
        {1999, 8, 15, 12, 0, 0},    /* the other century: the century bit is set */
        {2024, 2, 29, 6, 30, 15},   /* a leap day */
    };
    size_t i;

    ktest_begin("rtc: a moment through the registers comes back the same");

    for (i = 0; i < sizeof(moments) / sizeof(moments[0]); i++) {
        struct rtc_datetime in;
        struct rtc_datetime out;
        uint8_t raw[7];

        memset(&in, 0, sizeof(in));
        in.year = moments[i].year;
        in.month = moments[i].month;
        in.day = moments[i].day;
        in.hour = moments[i].hour;
        in.minute = moments[i].minute;
        in.second = moments[i].second;

        rtc_datetime_encode(&in, raw);
        rtc_datetime_decode(raw, &out);

        ktest_checks++;

        if (!out.valid || out.year != in.year || out.month != in.month || out.day != in.day ||
            out.hour != in.hour || out.minute != in.minute || out.second != in.second) {
            ktest_fail(__FILE__, __LINE__, "%04d-%02d-%02d came back different", in.year, in.month,
                       in.day);
            break;
        }
    }
}

static void test_weekday(void)
{
    ktest_begin("rtc: the day of the week follows the date");

    KT_CHECK_INT_EQ(rtc_weekday(2026, 10, 26), 1); /* a Monday */
    KT_CHECK_INT_EQ(rtc_weekday(2000, 1, 1), 6);   /* a Saturday */
    KT_CHECK_INT_EQ(rtc_weekday(1900, 1, 1), 1);   /* a Monday, before the leap rule of 1900 */
    KT_CHECK_INT_EQ(rtc_weekday(2024, 2, 29), 4);  /* a Thursday */
    KT_CHECK_INT_EQ(rtc_weekday(2026, 2, 28), 6);  /* a Saturday */
    KT_CHECK_INT_EQ(rtc_weekday(2026, 13, 1), -1); /* a month that does not exist */
}

static void test_parse(void)
{
    struct rtc_datetime dt;

    ktest_begin("rtc: the words of a person are exactly the words of the command");

    memset(&dt, 0, sizeof(dt));
    KT_CHECK(rtc_parse_date("2026-10-26", &dt));
    KT_CHECK_INT_EQ(dt.year, 2026);
    KT_CHECK_INT_EQ(dt.month, 10);
    KT_CHECK_INT_EQ(dt.day, 26);
    KT_CHECK_INT_EQ(dt.weekday, 1);

    memset(&dt, 0, sizeof(dt));
    KT_CHECK(rtc_parse_time("12:34:56", &dt));
    KT_CHECK_INT_EQ(dt.hour, 12);
    KT_CHECK_INT_EQ(dt.minute, 34);
    KT_CHECK_INT_EQ(dt.second, 56);

    /* The shape is checked by the parser, the sense by the ranges. */
    memset(&dt, 0, sizeof(dt));
    KT_CHECK(rtc_parse_date("2026-13-01", &dt));
    KT_CHECK(!rtc_datetime_in_range(&dt));

    memset(&dt, 0, sizeof(dt));
    KT_CHECK(rtc_parse_time("24:00:00", &dt));
    KT_CHECK(!rtc_datetime_in_range(&dt));

    memset(&dt, 0, sizeof(dt));
    KT_CHECK(!rtc_parse_date("26-10-2026", &dt));
    KT_CHECK(!rtc_parse_date("2026-1-26", &dt));
    KT_CHECK(!rtc_parse_date("2026-1x-26", &dt));
    KT_CHECK(!rtc_parse_date("2026-10-26 ", &dt));
    KT_CHECK(!rtc_parse_date(NULL, &dt));
    KT_CHECK(!rtc_parse_time("1:34:56", &dt));
    KT_CHECK(!rtc_parse_time("12:34", &dt));
    KT_CHECK(!rtc_parse_time("12-34-56", &dt));
    KT_CHECK(!rtc_parse_time(NULL, &dt));
}

static void test_format(void)
{
    struct rtc_datetime dt;
    char buf[32];
    char tiny[10];

    ktest_begin("rtc: the moment is written out as one line");

    memset(&dt, 0, sizeof(dt));
    rtc_parse_date("2026-10-26", &dt);
    rtc_parse_time("12:34:59", &dt);

    memset(buf, 0, sizeof(buf));
    rtc_format(&dt, buf, sizeof(buf));
    KT_CHECK_STR_EQ(buf, "2026-10-26 12:34:59");

    /* A buffer that is too small gets a line that fits, not a overflow. */
    memset(tiny, 0, sizeof(tiny));
    rtc_format(&dt, tiny, sizeof(tiny));
    KT_CHECK_STR_EQ(tiny, "2026-10-2");

    rtc_format(&dt, NULL, 0); /* nothing to write to: nothing happens */
    KT_CHECK(1);
}

/* The corpus of the tests, twice: the same number, the same answer. */
static void test_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    int checked = 0;

    ktest_begin("rtc: the numbers of the corpus, twice the same");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        double value = strtod(line, NULL);
        long long magnitude = (value < 0.0) ? (long long)(-value) : (long long)value;
        int second = (int)(magnitude % 60);
        int minute = (int)((magnitude / 60) % 60);
        int hour = (int)((magnitude / 3600) % 24);
        uint8_t raw[7];
        struct rtc_datetime first;
        struct rtc_datetime second_run;
        uint8_t again[7];

        memset(&first, 0, sizeof(first));
        first.year = 2026;
        first.month = 10;
        first.day = 26;
        first.hour = hour;
        first.minute = minute;
        first.second = second;

        rtc_datetime_encode(&first, raw);
        rtc_datetime_decode(raw, &first);
        rtc_datetime_encode(&first, again);
        rtc_datetime_decode(again, &second_run);

        ktest_checks++;
        checked++;

        if (!first.valid || second_run.hour != first.hour || second_run.minute != first.minute ||
            second_run.second != first.second) {
            ktest_fail(__FILE__, __LINE__, "%lld gave %02d:%02d:%02d and then something else",
                       magnitude, first.hour, first.minute, first.second);
            break;
        }
    }

    fclose(file);
    KT_CHECK(checked > 0);
}

void test_rtc(void)
{
    test_bcd();
    test_decode();
    test_low_voltage();
    test_garbage();
    test_calendar();
    test_round_trip();
    test_weekday();
    test_parse();
    test_format();
    test_corpus();
}
