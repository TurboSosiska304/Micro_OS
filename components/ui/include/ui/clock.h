/*
 * Micro OS — the clock of the desktop (ADR 0054, docs/desktop.md).
 *
 * The panel of the desk shows the time of the machine, and the format of it is
 * a key of the theme: `clock=24h` or `clock=12h`, and `utc_offset=` in minutes
 * east of UTC for a machine that lives in a zone. The file is `/etc/console`,
 * the same one that colours the console and the desk, so one file holds the
 * mood of the machine and the way it writes the hour.
 *
 * The time itself is the wall clock of the kernel (CMOS at the boot, NTP
 * later), which is UTC: this file adds the minutes of the zone and formats the
 * seconds of the day. It draws nothing and reads no file, so the host test
 * holds the format and the arithmetic without a machine.
 */
#ifndef MICRO_OS_UI_CLOCK_H
#define MICRO_OS_UI_CLOCK_H

#include <stdbool.h>

#include "kernel/types.h"

/* The minutes a zone is from UTC, as a clock keeps them: -720 (UTC-12) to
 * +840 (UTC+14). */
#define UI_CLOCK_OFFSET_MIN (-720)
#define UI_CLOCK_OFFSET_MAX 840

/* The way the panel writes the hour: `twelve` for "3:04:05 PM" and false for
 * "15:04:05", and the zone as minutes east of UTC. */
struct ui_clock {
    bool twelve;
    int offset_minutes;
};

/* The default of a machine without the keys: the twenty-four hours of the
 * console log and UTC. */
void ui_clock_default(struct ui_clock *clock);

/*
 * One line of a settings file: `clock=12h`, `clock=24h`, `utc_offset=-300`.
 * True when the line was one of the two keys (a value this version cannot read
 * leaves the field as it was and is still a line of the clock), false for the
 * other keys of /etc/console, which belong to the palette.
 */
bool ui_clock_line(struct ui_clock *clock, const char *line);

/*
 * `seconds` since 1970 as the panel writes the hour, into `out` (at least
 * UI_CLOCK_TEXT_MAX bytes): "15:04:05" or "3:04:05 PM". A moment before 1970
 * is the hour of the same day of the epoch and not a negative clock.
 */
#define UI_CLOCK_TEXT_MAX 12
void ui_clock_text(const struct ui_clock *clock, long seconds, char *out, k_size_t size);

#endif /* MICRO_OS_UI_CLOCK_H */
