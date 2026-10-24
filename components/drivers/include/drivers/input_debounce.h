/*
 * Micro OS — the debounce of the buttons, as pure arithmetic.
 *
 * The driver feeds the raw samples of the buttons into this little state
 * machine and tells a person only what survived the bounces. The file
 * knows nothing about GPIO, time sources or tasks — that is exactly why
 * the host tests can compile it and replay recorded samples through it.
 */
#ifndef MICRO_OS_DRIVERS_INPUT_DEBOUNCE_H
#define MICRO_OS_DRIVERS_INPUT_DEBOUNCE_H

#include "drivers/input.h"
#include "kernel/types.h"

/*
 * How long a level has to hold before it is believed: a mechanical
 * contact bounces for a few milliseconds, and every step back restarts
 * the wait for the quiet period.
 */
#define INPUT_DEBOUNCE_MS 20

/* One channel: the level the events are told with, and the level being tested. */
struct input_debounce_button {
    bool stable;    /* the level that has survived the debounce */
    bool candidate; /* the raw level that is being tested */
    k_u32_t since;  /* when the candidate was first seen (milliseconds) */
};

struct input_debounce {
    struct input_debounce_button buttons[INPUT_BUTTON_COUNT];
};

/* Every channel starts released, so a session always begins with no press. */
void input_debounce_init(struct input_debounce *d);

/*
 * Feed one raw sample of one button taken at now_ms.
 *
 * A raw level becomes the truth only when the button has held it for
 * INPUT_DEBOUNCE_MS without a single step back; a bounce restarts the
 * wait. Returns 1 when the stable level changes — that is an event, and
 * the new level is written to *pressed — and 0 when there is nothing to
 * say. Returns -1 and touches nothing when the button is out of range.
 *
 * The clock is the caller's: the caller passes its own milliseconds, so
 * the result depends only on the samples and their times (the same data
 * gives the same result).
 */
int input_debounce_sample(struct input_debounce *d, int button, bool pressed, k_u32_t now_ms,
                          bool *pressed_out);

/* The level that the events are told with right now (for tests and tools). */
bool input_debounce_stable(const struct input_debounce *d, int button);

#endif /* MICRO_OS_DRIVERS_INPUT_DEBOUNCE_H */
