/*
 * Micro OS — the curve of the battery (the pure part).
 *
 * See drivers/battery_soc.h: no ADC, no clock, no tasks — only a
 * voltage and the answer of the curve, so the host tests can replay
 * voltages through it.
 */
#include "drivers/battery_soc.h"

/* One point of the curve: the voltage of a resting cell, and the charge it means. */
struct soc_point {
    int mv;
    int percent;
};

/*
 * The curve of one lithium cell under a light load, points going up:
 * the long plateau of the chemistry in the middle (a few tens of
 * millivolts carry tens of percent) and the cliffs at both ends, where
 * the last milliamps live and where the cell gives up quickly.
 *
 * It is an approximation of a resting cell and nothing more — the
 * honest number for a cell the board is not drawing from right now.
 */
static const struct soc_point s_curve[] = {
    {BATTERY_EMPTY_MV, 0},
    {3450, 5},
    {3680, 10},
    {3740, 20},
    {3770, 30},
    {3790, 40},
    {3820, 50},
    {3870, 60},
    {3920, 70},
    {3980, 80},
    {4060, 90},
    {BATTERY_FULL_MV, 100},
};

#define CURVE_POINTS ((int)(sizeof(s_curve) / sizeof(s_curve[0])))

int battery_soc_percent(int mv)
{
    int i;

    if (mv <= s_curve[0].mv) {
        return 0;
    }

    if (mv >= s_curve[CURVE_POINTS - 1].mv) {
        return 100;
    }

    for (i = 1; i < CURVE_POINTS; i++) {
        if (mv < s_curve[i].mv) {
            const struct soc_point *low = &s_curve[i - 1];
            const struct soc_point *high = &s_curve[i];

            /* The straight line between the two neighbours of the voltage. */
            return low->percent +
                   (mv - low->mv) * (high->percent - low->percent) / (high->mv - low->mv);
        }
    }

    return 100; /* unreachable: the last point was answered by the clamps above */
}
