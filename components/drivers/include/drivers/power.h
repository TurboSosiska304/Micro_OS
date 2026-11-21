/*
 * Micro OS — the power of the StickC Plus2: the hold line and deep sleep.
 *
 * The board has no PMIC. Its power is a latch that the ESP32 keeps closed
 * through HOLD (G4): the line must be driven high at every boot, including
 * the boot that follows a deep sleep, or the board cuts its own power. The
 * wake line of the chip is the one of BUTTON C (G35): the button closes that
 * net to ground, and the timer of the chip is the other way back. The alarm
 * of the RTC is not on that line — it drives the latch and can switch on a
 * board that is off — so it does not wake a chip that is asleep.
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
 * Deep sleep. With seconds > 0 the timer wakes the board; BUTTON C wakes it
 * in any case, sooner if it is pressed. The call does not return on success;
 * a negative answer is the failure to arm the wake-up sources.
 */
int drv_power_sleep(k_u32_t seconds);

/*
 * Cut the power by dropping HOLD. On a battery the board goes away and this
 * call never returns; with the cable plugged in the board keeps running and
 * the call answers 0 after the line is restored (there is no software way to
 * switch off a board that is fed by USB).
 */
int drv_power_off(void);

/* Register the module of the hardware doctor and the commands of the shell. */
int drv_power_register(void);

#endif /* MICRO_OS_DRIVERS_POWER_H */
