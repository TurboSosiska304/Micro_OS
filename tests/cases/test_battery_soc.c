/*
 * Micro OS — the curve of the battery.
 *
 * The curve is checked with the voltages a person can compute by hand:
 * the ends of it (empty and full), a few points straight off the table,
 * a line between two points, and a walk through every millivolt from 0
 * to 4500 to see that the answer never walks backwards and never leaves
 * 0..100. The last case runs the real numbers of the corpus through it
 * twice: the same voltage has to give the same percent.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>

#include "drivers/battery_soc.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

/* Outside the curve there is nothing but the ends of it. */
static void test_ends(void)
{
    ktest_begin("battery: below the curve is empty, above it is full");

    KT_CHECK_INT_EQ(battery_soc_percent(-100), 0);
    KT_CHECK_INT_EQ(battery_soc_percent(0), 0);
    KT_CHECK_INT_EQ(battery_soc_percent(BATTERY_EMPTY_MV - 1), 0);
    KT_CHECK_INT_EQ(battery_soc_percent(BATTERY_EMPTY_MV), 0);
    KT_CHECK_INT_EQ(battery_soc_percent(BATTERY_FULL_MV), 100);
    KT_CHECK_INT_EQ(battery_soc_percent(BATTERY_FULL_MV + 1), 100);
    KT_CHECK_INT_EQ(battery_soc_percent(100000), 100);
}

/* The points of the curve say what they say. */
static void test_points(void)
{
    ktest_begin("battery: the points of the curve are what they say");

    KT_CHECK_INT_EQ(battery_soc_percent(3680), 10);
    KT_CHECK_INT_EQ(battery_soc_percent(3770), 30);
    KT_CHECK_INT_EQ(battery_soc_percent(3820), 50);
    KT_CHECK_INT_EQ(battery_soc_percent(3980), 80);
    KT_CHECK_INT_EQ(battery_soc_percent(4060), 90);
}

/* Between two points the answer is a straight line, computed by hand. */
static void test_between(void)
{
    ktest_begin("battery: between two points the answer is a straight line");

    /* 90% at 4060, 100% at 4200: 4130 is exactly in the middle → 95. */
    KT_CHECK_INT_EQ(battery_soc_percent(4130), 95);

    /* 0% at 3000, 5% at 3450: 3225 is halfway → 1125/450 → 2, the rest is cut. */
    KT_CHECK_INT_EQ(battery_soc_percent(3225), 2);

    /* One millivolt past the empty end is still on the first line: 1 → 5/450 → 0. */
    KT_CHECK_INT_EQ(battery_soc_percent(BATTERY_EMPTY_MV + 1), 0);
}

/*
 * A walk through the whole range: every millivolt from 0 to 4500 has to
 * answer inside 0..100, and a higher voltage never gives a lower
 * percent — not once in four and a half thousand steps.
 */
static void test_monotonic(void)
{
    int mv;
    int previous = -1;

    ktest_begin("battery: a higher voltage is never a lower percent");

    for (mv = 0; mv <= 4500; mv++) {
        int percent = battery_soc_percent(mv);

        ktest_checks++;

        if (percent < 0 || percent > 100 || percent < previous) {
            ktest_fail(__FILE__, __LINE__, "at %d mV: %d%% after %d%%", mv, percent, previous);
            return;
        }

        previous = percent;
    }

    KT_CHECK_INT_EQ(previous, 100);
}

/* The corpus: the real numbers, through the curve, twice the same. */
static void test_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    int checked = 0;

    ktest_begin("battery: the numbers of the corpus, twice the same");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        double value = strtod(line, NULL);
        long long magnitude = (value < 0.0) ? (long long)(-value) : (long long)value;
        int mv = 2500 + (int)(magnitude % 2500); /* 2500..4999: through the whole curve */
        int first = battery_soc_percent(mv);
        int second = battery_soc_percent(mv);

        ktest_checks++;
        checked++;

        if (first < 0 || first > 100 || first != second) {
            ktest_fail(__FILE__, __LINE__, "%d mV gave %d%% and %d%%", mv, first, second);
            break;
        }
    }

    fclose(file);
    KT_CHECK(checked > 0);
}

void test_battery_soc(void)
{
    test_ends();
    test_points();
    test_between();
    test_monotonic();
    test_corpus();
}
