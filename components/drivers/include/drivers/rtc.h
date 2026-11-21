/*
 * Micro OS — the alarm of the RTC (BM8563).
 *
 * The chip compares the alarm registers with the running time and pulls its
 * INT line low when they match. On the StickC Plus2 that line drives the
 * latch of the power supply: an alarm can switch on a board that is off, and
 * a board that is on only finds the flag set — the line is not the EXT1 wake
 * line of the chip, so an alarm does not wake a chip that is in deep sleep
 * (the timer and BUTTON C do, see drivers/power.h). The boot reads the flag
 * to say that an alarm went off, and clears it, because the INT line stays
 * low until somebody answers it.
 *
 * A field that is not wanted is left at -1 and the chip does not compare it,
 * which is exactly the "AE" bit of the alarm registers. The alarm of this
 * chip is a match of a minute, an hour and a day of the month; there is no
 * month and no year, so a field that is set repeats: an alarm on 12:34 with
 * no day fires every day at 12:34 until it is cleared or the board restarts.
 */
#ifndef MICRO_OS_DRIVERS_RTC_H
#define MICRO_OS_DRIVERS_RTC_H

#include "kernel/types.h"

struct drv_rtc_alarm {
    int hour;   /* 0..23, -1 = any hour */
    int minute; /* 0..59, -1 = any minute */
    int day;    /* 1..31 of the month, -1 = any day */
};

/*
 * Set the alarm and enable it. Returns 0 on success, -1 when there is no
 * clock, -2 for a field out of range, -3 when the chip did not take the
 * registers, -4 when the interrupt could not be enabled.
 */
int drv_rtc_alarm_set(const struct drv_rtc_alarm *alarm);

/*
 * Read the alarm back from the chip: the fields it compares (-1 for the ones
 * it does not) and whether its flag is set, which means the alarm went off
 * and nobody has answered it yet. The flag stays set until it is cleared —
 * and while it is set the INT line stays low, which holds the power latch of
 * the board closed (see `power off`). Returns 0 or -1 when there is no clock.
 */
int drv_rtc_alarm_read(struct drv_rtc_alarm *alarm, bool *fired);

/* Disable the alarm and clear its flag: the INT line is released. */
int drv_rtc_alarm_clear(void);

/*
 * Did the alarm go off before this boot? The driver reads the flag while it
 * brings the chip up and then clears it, so the boot can say that an alarm
 * went off without carrying the flag across the reset.
 */
bool drv_rtc_alarm_was_fired(void);

#endif /* MICRO_OS_DRIVERS_RTC_H */
