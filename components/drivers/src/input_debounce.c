/*
 * Micro OS — the debounce of the buttons (the pure part).
 *
 * See drivers/input_debounce.h: no hardware, no clock, no tasks - only
 * samples and their timestamps, so that the host tests can replay them.
 */
#include "drivers/input_debounce.h"

void input_debounce_init(struct input_debounce *d)
{
    int i;

    for (i = 0; i < INPUT_BUTTON_COUNT; i++) {
        d->buttons[i].stable = false;
        d->buttons[i].candidate = false;
        d->buttons[i].since = 0;
    }
}

int input_debounce_sample(struct input_debounce *d, int button, bool pressed, k_u32_t now_ms,
                          bool *pressed_out)
{
    struct input_debounce_button *b;

    if (d == NULL || pressed_out == NULL || button < 0 || button >= INPUT_BUTTON_COUNT) {
        return -1;
    }

    b = &d->buttons[button];

    if (pressed != b->candidate) {
        /*
         * The raw level turned: whatever was being tested is abandoned and
         * the quiet period starts again. A contact that bounces every
         * millisecond therefore never produces a single event.
         */
        b->candidate = pressed;
        b->since = now_ms;
        return 0;
    }

    if (pressed != b->stable && (k_u32_t)(now_ms - b->since) >= INPUT_DEBOUNCE_MS) {
        /*
         * The level has held for long enough: the events may believe it.
         * The subtraction is unsigned, so the clock may roll over - what
         * matters is the distance between two samples, not their values.
         */
        b->stable = pressed;
        *pressed_out = pressed;
        return 1;
    }

    return 0;
}

bool input_debounce_stable(const struct input_debounce *d, int button)
{
    if (d == NULL || button < 0 || button >= INPUT_BUTTON_COUNT) {
        return false;
    }

    return d->buttons[button].stable;
}
