/*
 * Micro OS — the power of the StickC Plus2: the hold line and deep sleep.
 *
 * The board has no PMIC. Its power is a latch that the ESP32 keeps closed
 * through HOLD (G4): the line must be driven high at every boot, including
 * the boot that follows a deep sleep, or the board cuts its own power. The
 * wake line of the chip is the one of BUTTON C (G35): the button closes that
 * net to ground, and the timer of the chip is the other way back.
 *
 * The alarm of the RTC is not on that line — its INT drives the latch and can
 * switch on a board that is off — but a sleep can still end with it: the
 * driver asks the clock for the seconds to the next match and arms the same
 * timer of the chip for that moment, so the chip wakes when the alarm would
 * have fired and the flag of the clock confirms it. Without that the alarm
 * would sit in the clock while the board slept through it, which is exactly
 * what the first firmware did.
 */
#ifndef MICRO_OS_DRIVERS_POWER_H
#define MICRO_OS_DRIVERS_POWER_H

#include "kernel/types.h"

/*
 * Drive HOLD (G4) high: from this call on the board stays powered by
 * itself. The boot calls it before anything else; the driver module calls
 * it again, which costs nothing.
 */
int drv_power_hold(void);

/* Why the board came up: "power-on", "timer", "ext1", ... (a static string). */
const char *drv_power_wakeup_reason(void);

/*
 * Deep sleep for `seconds`: the timer wakes the board, and an armed alarm of
 * the clock wakes it sooner, at the moment the alarm fires. BUTTON C wakes it
 * in any case, sooner if it is pressed. The call does not return on success;
 * a negative answer is the failure to arm the wake-up sources.
 */
int drv_power_sleep(k_u32_t seconds);

/*
 * Deep sleep until the next match of the alarm of the clock. Returns -1 when
 * there is no clock, -2 when no alarm is armed, -3 when the clock has no time
 * to count from, -4 when the alarm matches nothing in the next month; the
 * other negative answers are the failure of drv_power_sleep().
 */
int drv_power_sleep_until_alarm(void);

/*
 * Cut the power by dropping HOLD. On a battery the board goes away and this
 * call never returns; with the cable plugged in the board keeps running and
 * the call answers 0 after the line is restored (there is no software way to
 * switch off a board that is fed by USB).
 */
int drv_power_off(void);

/*
 * The off of the clean end (ADR 0050): hold HOLD, arm BUTTON C as the only
 * wake source and go to deep sleep. The board is off until a person presses
 * the button; there is no timer and no alarm. The call does not return on
 * success, and a negative answer is a wake source that could not be armed -
 * the caller then drops the latch (drv_power_off) rather than sleep with no
 * way back.
 */
int drv_power_deep_off(void);

/* Register the module of the hardware doctor and the commands of the shell. */
int drv_power_register(void);

#endif /* MICRO_OS_DRIVERS_POWER_H */
