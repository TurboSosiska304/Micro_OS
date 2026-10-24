/*
 * Micro OS — the debounce of the buttons.
 *
 * The samples below are the data the state machine is fed with: a clean
 * press, a bouncing contact, a chatter that never settles, a short dip in
 * the middle of a hold, two buttons at once, and the clock of the system
 * rolling over. The last case replays the real numbers of the corpus
 * through it and compares the events with an independent count made in
 * "holds" (the level and how many samples it kept), so the same data
 * always gives the same result.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/input.h"
#include "drivers/input_debounce.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

/* One sample of one button, taken at a moment (milliseconds). */
struct sample {
    int button;
    bool pressed;
    k_u32_t at;
};

/* One event the state machine told about. */
struct fired {
    int button;
    bool pressed;
    k_u32_t at;
};

/* The event at the given index, as it has to be. */
#define KT_EVENT_EQ(got, index, btn, press, when)                                                  \
    do {                                                                                           \
        ktest_checks++;                                                                            \
        if (!((got)[index].button == (btn) && (got)[index].pressed == (press) &&                   \
              (got)[index].at == (when))) {                                                        \
            ktest_fail(__FILE__, __LINE__, "event %d: got %d/%d/%u, expected %d/%d/%u",            \
                       (int)(index), (got)[index].button, (int)(got)[index].pressed,               \
                       (unsigned)(got)[index].at, (int)(btn), (int)(press), (unsigned)(when));     \
        }                                                                                          \
    } while (0)

/* Replay a trace; the state after it is left in *state for inspection. */
static int run(const struct sample *trace, int count, struct fired *out, int max,
               struct input_debounce *state)
{
    int fired_count = 0;
    int i;

    input_debounce_init(state);

    for (i = 0; i < count; i++) {
        bool pressed = false;
        int rc =
            input_debounce_sample(state, trace[i].button, trace[i].pressed, trace[i].at, &pressed);

        if (rc == 1) {
            if (fired_count < max) {
                out[fired_count].button = trace[i].button;
                out[fired_count].pressed = pressed;
                out[fired_count].at = trace[i].at;
            }
            fired_count++;
        }
    }

    return fired_count;
}

/* A press and a release without a single bounce in them. */
static void test_clean_press(void)
{
    static const struct sample trace[] = {
        {INPUT_BUTTON_A, false, 0},   {INPUT_BUTTON_A, false, 10},  {INPUT_BUTTON_A, false, 20},
        {INPUT_BUTTON_A, true, 100},  {INPUT_BUTTON_A, true, 110},  {INPUT_BUTTON_A, true, 120},
        {INPUT_BUTTON_A, true, 130},  {INPUT_BUTTON_A, false, 200}, {INPUT_BUTTON_A, false, 210},
        {INPUT_BUTTON_A, false, 220}, {INPUT_BUTTON_A, false, 230},
    };
    struct input_debounce state;
    struct fired got[4];
    int n;

    ktest_begin("debounce: a clean press and a clean release");

    n = run(trace, (int)(sizeof(trace) / sizeof(trace[0])), got, 4, &state);

    KT_CHECK_INT_EQ(n, 2);
    if (n == 2) {
        KT_EVENT_EQ(got, 0, INPUT_BUTTON_A, true, 120);
        KT_EVENT_EQ(got, 1, INPUT_BUTTON_A, false, 220);
    }
    KT_CHECK(!input_debounce_stable(&state, INPUT_BUTTON_A));
}

/*
 * The contact of a real button bounces: it closes and opens for a few
 * milliseconds before it stays closed. None of that may be told, and the
 * settled press comes exactly INPUT_DEBOUNCE_MS after the last bounce.
 */
static const struct sample g_bounce[] = {
    {INPUT_BUTTON_A, true, 100},  {INPUT_BUTTON_A, false, 101}, {INPUT_BUTTON_A, true, 102},
    {INPUT_BUTTON_A, false, 103}, {INPUT_BUTTON_A, true, 104},  {INPUT_BUTTON_A, false, 105},
    {INPUT_BUTTON_A, true, 106},  {INPUT_BUTTON_A, false, 107}, {INPUT_BUTTON_A, true, 110},
    {INPUT_BUTTON_A, true, 115},  {INPUT_BUTTON_A, true, 125},  {INPUT_BUTTON_A, true, 130},
};

static void test_bounce(void)
{
    struct input_debounce state;
    struct fired got[4];
    int n;

    ktest_begin("debounce: a bouncing contact tells only the settled press");

    n = run(g_bounce, (int)(sizeof(g_bounce) / sizeof(g_bounce[0])), got, 4, &state);

    KT_CHECK_INT_EQ(n, 1);
    if (n == 1) {
        KT_EVENT_EQ(got, 0, INPUT_BUTTON_A, true, 130);
    }
    KT_CHECK(input_debounce_stable(&state, INPUT_BUTTON_A));
}

/* The same data, twice: the second run has to give the same result. */
static void test_same_data_same_result(void)
{
    struct input_debounce first_state;
    struct input_debounce second_state;
    struct fired first[4];
    struct fired second[4];
    int a;
    int b;
    int i;

    ktest_begin("debounce: the same samples give the same events");

    a = run(g_bounce, (int)(sizeof(g_bounce) / sizeof(g_bounce[0])), first, 4, &first_state);
    b = run(g_bounce, (int)(sizeof(g_bounce) / sizeof(g_bounce[0])), second, 4, &second_state);

    KT_CHECK_INT_EQ(a, b);
    for (i = 0; i < a && i < b && i < 4; i++) {
        /* The fields, one by one: the padding of the struct is not data. */
        KT_CHECK(first[i].button == second[i].button && first[i].pressed == second[i].pressed &&
                 first[i].at == second[i].at);
    }
}

/* A chatter that never settles must never produce an event. */
static void test_chatter(void)
{
    struct sample trace[512];
    struct input_debounce state;
    struct fired got[8];
    k_u32_t at;
    bool level = true;
    int count = 0;
    int i;
    int n;

    ktest_begin("debounce: a chatter that never settles is never told");

    for (at = 100; at < 600 && count < 500; at++) {
        trace[count].button = INPUT_BUTTON_A;
        trace[count].pressed = level;
        trace[count].at = at;
        count++;
        level = !level;
    }

    /* And the rest after the chatter: the button is released again. */
    for (i = 0; i < 3 && count < 512; i++) {
        trace[count].button = INPUT_BUTTON_A;
        trace[count].pressed = false;
        trace[count].at = 600 + (k_u32_t)i * 10;
        count++;
    }

    n = run(trace, count, got, 8, &state);

    KT_CHECK_INT_EQ(n, 0);
    KT_CHECK(!input_debounce_stable(&state, INPUT_BUTTON_A));
}

/* A short dip in the middle of a hold is not a release. */
static void test_glitch(void)
{
    static const struct sample trace[] = {
        {INPUT_BUTTON_A, true, 100},  {INPUT_BUTTON_A, true, 120},  {INPUT_BUTTON_A, false, 500},
        {INPUT_BUTTON_A, true, 510},  {INPUT_BUTTON_A, true, 520},  {INPUT_BUTTON_A, true, 600},
        {INPUT_BUTTON_A, false, 600}, {INPUT_BUTTON_A, false, 610}, {INPUT_BUTTON_A, false, 620},
    };
    struct input_debounce state;
    struct fired got[4];
    int n;

    ktest_begin("debounce: a dip shorter than the window is not a release");

    n = run(trace, (int)(sizeof(trace) / sizeof(trace[0])), got, 4, &state);

    KT_CHECK_INT_EQ(n, 2);
    if (n == 2) {
        KT_EVENT_EQ(got, 0, INPUT_BUTTON_A, true, 120);
        KT_EVENT_EQ(got, 1, INPUT_BUTTON_A, false, 620);
    }
}

/* Two buttons pressed at once are two separate events, each debounced on its own. */
static void test_two_buttons(void)
{
    static const struct sample trace[] = {
        {INPUT_BUTTON_B, true, 100}, {INPUT_BUTTON_A, true, 105}, {INPUT_BUTTON_B, true, 110},
        {INPUT_BUTTON_A, true, 115}, {INPUT_BUTTON_B, true, 120}, {INPUT_BUTTON_A, true, 125},
        {INPUT_BUTTON_A, true, 135},
    };
    struct input_debounce state;
    struct fired got[4];
    int n;

    ktest_begin("debounce: two buttons at once are two events");

    n = run(trace, (int)(sizeof(trace) / sizeof(trace[0])), got, 4, &state);

    KT_CHECK_INT_EQ(n, 2);
    if (n == 2) {
        KT_EVENT_EQ(got, 0, INPUT_BUTTON_B, true, 120);
        KT_EVENT_EQ(got, 1, INPUT_BUTTON_A, true, 125);
    }
    KT_CHECK(input_debounce_stable(&state, INPUT_BUTTON_A));
    KT_CHECK(input_debounce_stable(&state, INPUT_BUTTON_B));
}

/*
 * The clock of the system rolls over every 2^32 milliseconds (every 49
 * days): the distance between two samples is what counts, not their
 * values, so a press across the roll has to be seen as any other.
 */
static void test_clock_wrap(void)
{
    static const struct sample trace[] = {
        {INPUT_BUTTON_A, true, 0xFFFFFFF0u},
        {INPUT_BUTTON_A, true, 0x00000004u},
    };
    struct input_debounce state;
    struct fired got[4];
    int n;

    ktest_begin("debounce: a press across the roll of the clock");

    n = run(trace, (int)(sizeof(trace) / sizeof(trace[0])), got, 4, &state);

    KT_CHECK_INT_EQ(n, 1);
    if (n == 1) {
        KT_EVENT_EQ(got, 0, INPUT_BUTTON_A, true, 4);
    }
}

/* Nothing out of range is touched, and nothing out of range is believed. */
static void test_bad_arguments(void)
{
    struct input_debounce state;
    bool pressed = true;

    ktest_begin("debounce: the arguments out of range change nothing");

    input_debounce_init(&state);

    KT_CHECK_INT_EQ(input_debounce_sample(&state, -1, true, 100, &pressed), -1);
    KT_CHECK_INT_EQ(input_debounce_sample(&state, INPUT_BUTTON_COUNT, true, 100, &pressed), -1);
    KT_CHECK_INT_EQ(input_debounce_sample(NULL, 0, true, 100, &pressed), -1);
    KT_CHECK_INT_EQ(input_debounce_sample(&state, 0, true, 100, NULL), -1);
    KT_CHECK(pressed == true); /* the output was not written */
    KT_CHECK(!input_debounce_stable(&state, INPUT_BUTTON_COUNT));
    KT_CHECK(!input_debounce_stable(NULL, 0));
}

/*
 * The corpus: the real numbers of the file decide how long the button
 * holds its level — 1 to 40 samples of 5 ms, from 5 to 200 milliseconds,
 * which covers both sides of the 20 ms window. The events are compared
 * with an independent count that only knows holds: a hold is told when
 * it kept its level for five samples or more and its level is not the
 * one that is already settled.
 */
#define CORPUS_MAX 600

static void test_corpus(void)
{
    static bool levels[CORPUS_MAX];
    static int lengths[CORPUS_MAX];
    struct input_debounce state;
    struct fired got[CORPUS_MAX];
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    int holds = 0;
    int step = 0;
    int i;
    int n = 0;
    bool level = false;
    bool settled = false;
    int expected = 0;

    ktest_begin("debounce: the samples from the corpus, counted twice");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL && holds < CORPUS_MAX) {
        double value = strtod(line, NULL);
        long long magnitude = (value < 0.0) ? (long long)(-value) : (long long)value;

        lengths[holds] = 1 + (int)(magnitude % 40); /* 1..40 samples */
        level = !level;
        levels[holds] = level;
        holds++;
    }

    fclose(file);
    KT_CHECK(holds > 0);
    if (holds == 0) {
        return;
    }

    /* The samples of every hold, one every 5 milliseconds, as they come. */
    input_debounce_init(&state);
    for (i = 0; i < holds; i++) {
        int s;

        for (s = 0; s < lengths[i]; s++) {
            bool pressed = false;
            k_u32_t at = (k_u32_t)step * 5;

            if (input_debounce_sample(&state, INPUT_BUTTON_A, levels[i], at, &pressed) == 1) {
                if (n < CORPUS_MAX) {
                    got[n].button = INPUT_BUTTON_A;
                    got[n].pressed = pressed;
                    got[n].at = at;
                }
                n++;
            }

            step++;
        }
    }

    /* The count in holds, made without the state machine. */
    {
        struct fired want[CORPUS_MAX];
        int start_step = 0;

        for (i = 0; i < holds; i++) {
            if (lengths[i] >= 5 && levels[i] != settled) {
                want[expected].button = INPUT_BUTTON_A;
                want[expected].pressed = levels[i];
                want[expected].at = (k_u32_t)(start_step + 4) * 5;
                expected++;
                settled = levels[i];
            }
            start_step += lengths[i];
        }

        KT_CHECK_INT_EQ(n, expected);
        if (n == expected && n <= CORPUS_MAX) {
            for (i = 0; i < n; i++) {
                KT_EVENT_EQ(got, i, want[i].button, want[i].pressed, want[i].at);
            }
        }
    }
}

void test_input_debounce(void)
{
    test_clean_press();
    test_bounce();
    test_same_data_same_result();
    test_chatter();
    test_glitch();
    test_two_buttons();
    test_clock_wrap();
    test_bad_arguments();
    test_corpus();
}
