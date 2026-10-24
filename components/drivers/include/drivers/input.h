/*
 * Micro OS — the buttons A, B and C as events.
 *
 * The buttons are read by whoever waits for them (see drv_input_wait):
 * there is no interrupt and no background task, so the board spends
 * nothing at all when nobody listens. The waiter samples the pins every
 * INPUT_POLL_MS and puts the samples through the debounce, therefore an
 * event is never earlier than INPUT_DEBOUNCE_MS + INPUT_POLL_MS after
 * the press, and a press shorter than that may not be seen at all.
 */
#ifndef MICRO_OS_DRIVERS_INPUT_H
#define MICRO_OS_DRIVERS_INPUT_H

#include "kernel/types.h"

/* The buttons, in the order the events come. */
#define INPUT_BUTTON_A 0
#define INPUT_BUTTON_B 1
#define INPUT_BUTTON_C 2
#define INPUT_BUTTON_COUNT 3

/* How often the waiter samples the pins (milliseconds). */
#define INPUT_POLL_MS 10

/* The timeout of drv_input_wait that means "wait forever". */
#define INPUT_WAIT_FOREVER ((k_u32_t) - 1)

/* One press or one release of one button. */
struct input_event {
    k_u64_t time_ms;  /* the moment it happened (milliseconds since boot) */
    k_u8_t button;    /* INPUT_BUTTON_A, INPUT_BUTTON_B or INPUT_BUTTON_C */
    k_u8_t pressed;   /* 1: the button went down, 0: it came up */
    k_u16_t reserved; /* zero */
};

/*
 * Wait for the next debounced event. Returns 0 with the event in *out, or
 * -1 when timeout_ms passed without one (INPUT_WAIT_FOREVER waits for as
 * long as it takes).
 *
 * The state of the buttons lives between the waits and not in them: a
 * held button is one press, whoever reads it, and the release is told
 * after the press. The event goes to the first waiter that samples the
 * change — the waits share one reality, like the buttons they read.
 */
int drv_input_wait(struct input_event *out, k_u32_t timeout_ms);

#endif /* MICRO_OS_DRIVERS_INPUT_H */
