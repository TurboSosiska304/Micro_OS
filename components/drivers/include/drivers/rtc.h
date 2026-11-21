/*
 * Micro OS — the alarm of the RTC (BM8563).
 *
 * The chip compares the alarm registers with the running time and pulls its
 * INT line low when they match. On the StickC Plus2 that line drives the
 * latch of the power supply: an alarm can switch on a board that is off, and
 * a board that is on only finds the flag set. The line is not the EXT1 wake
 * line of the chip, so the INT alone cannot wake a chip that is in deep
 * sleep - the firmware does what the wiring cannot: a sleep arms the timer of
 * the chip for the moment of the alarm (see drivers/power.h), and the flag of
 * the clock confirms on the next boot that the wake was the alarm.
 *
 * The alarm lives in the clock, not in the store: it survives a restart, a
 * formatted volume, a factory reset and a dead battery of the chip, and the
 * boot keeps it armed instead of wiping the control registers. The boot reads
 * the flag to say that an alarm went off and clears the flag, because the INT
 * line stays low until somebody answers it.
 *
 * A field that is not wanted is left at -1 and the chip does not compare it,
 * which is exactly the "AE" bit of the alarm registers. The alarm of this
 * chip is a match of a minute, an hour and a day of the month; there is no
 * month and no year, so a field that is set repeats: an alarm on 12:34 with
 * no day fires every day at 12:34 until it is cleared.
 */
#ifndef MICRO_OS_DRIVERS_RTC_H
#define MICRO_OS_DRIVERS_RTC_H

#include "drivers/rtc_time.h"
#include "kernel/types.h"

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

/*
 * The seconds from the time of the chip to the next match of the armed
 * alarm, which is the length of a sleep that ends with the alarm. Returns 0
 * and fills *seconds, -1 when there is no clock, -2 when no alarm is armed,
 * -3 when the clock has no time to count from (a dead backup battery), and
 * -4 when the alarm matches nothing in the next month (an alarm on the 31st
 * asked in February).
 */
int drv_rtc_alarm_seconds_ahead(k_u32_t *seconds);

/* Disable the alarm and clear its flag: the INT line is released. */
int drv_rtc_alarm_clear(void);

/*
 * Did the alarm go off before this boot? The driver reads the flag while it
 * brings the chip up and then clears it, so the boot can say that an alarm
 * went off without carrying the flag across the reset.
 */
bool drv_rtc_alarm_was_fired(void);

/* Was an alarm armed when the chip came up? An alarm that fired with no
 * alarm armed is a flag left by a day the battery went away. */
bool drv_rtc_alarm_was_armed(void);

#endif /* MICRO_OS_DRIVERS_RTC_H */
