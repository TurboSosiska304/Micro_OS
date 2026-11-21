/*
 * Micro OS — the arithmetic of the sound detector.
 *
 * The whole point of the detector is that silence does not start it: a room
 * reads a few counts of RMS at every window, and one odd window of the
 * microphone must not look like a clap. The tests walk the runs of the state
 * machine - a single window, two in a row, a fade through the band, a sound
 * that goes away and comes back - and the counts of the events are what the
 * assertion is about.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "drivers/sound_detect.h"

/* The threshold of the tests: a quiet room of this board reads about 20. */
#define THRESHOLD 200u

static void test_quiet(void)
{
    struct sound_detector detector;
    unsigned window;

    ktest_begin("sound: a quiet room never starts the detector");

    sound_detector_init(&detector, THRESHOLD, 2, 4);

    /* A minute of silence: the RMS of a real quiet room, and a window that
     * is high but not high enough. */
    for (window = 0; window < 1200; window++) {
        unsigned rms = (window % 3 == 0) ? 20u : (window % 3 == 1 ? 8u : 199u);

        KT_CHECK(!sound_detector_feed(&detector, rms, rms * 3));

        if (detector.events != 0) {
            ktest_fail(__FILE__, __LINE__, "the window %u of silence began an event", window);
            return;
        }
    }

    KT_CHECK(!sound_detector_hearing(&detector));
    KT_CHECK_INT_EQ(detector.events, 0);
}

static void test_one_window(void)
{
    struct sound_detector detector;

    ktest_begin("sound: one window above the threshold is not a sound");

    sound_detector_init(&detector, THRESHOLD, 2, 4);

    /* A single spike - a step, a knock on the table - changes nothing. */
    KT_CHECK(!sound_detector_feed(&detector, THRESHOLD * 4, 9000));
    KT_CHECK(!sound_detector_hearing(&detector));
    KT_CHECK_INT_EQ(detector.events, 0);

    /* The window after it is quiet again, so the run is broken. */
    KT_CHECK(!sound_detector_feed(&detector, 10, 40));
    KT_CHECK(!sound_detector_hearing(&detector));

    /* Two windows in a row are a sound. */
    KT_CHECK(!sound_detector_feed(&detector, 800, 3000));
    KT_CHECK(sound_detector_feed(&detector, 900, 3200));
    KT_CHECK(sound_detector_hearing(&detector));
    KT_CHECK_INT_EQ(detector.events, 1);

    /* The event continues without a second one. */
    KT_CHECK(!sound_detector_feed(&detector, 1000, 3500));
    KT_CHECK_INT_EQ(detector.events, 1);
}

static void test_band(void)
{
    struct sound_detector detector;

    ktest_begin("sound: a fading sound stays a sound until it is well below");

    sound_detector_init(&detector, THRESHOLD, 2, 4);

    (void)sound_detector_feed(&detector, 500, 2000);
    KT_CHECK(sound_detector_feed(&detector, 500, 2000));
    KT_CHECK(sound_detector_hearing(&detector));

    /* Just under the threshold is inside the band: the word is fading, and
     * the detector keeps hearing it. A detector that let go here would
     * count every syllable of a sentence as an event. */
    KT_CHECK(!sound_detector_feed(&detector, THRESHOLD - 1, 900));
    KT_CHECK(!sound_detector_feed(&detector, THRESHOLD / 2, 700));
    KT_CHECK(sound_detector_hearing(&detector));
    KT_CHECK_INT_EQ(detector.events, 1);

    /* Well below it, four windows in a row: the event ends. */
    KT_CHECK(!sound_detector_feed(&detector, 50, 200));
    KT_CHECK(!sound_detector_feed(&detector, 50, 200));
    KT_CHECK(!sound_detector_feed(&detector, 50, 200));
    KT_CHECK(sound_detector_hearing(&detector));
    KT_CHECK(!sound_detector_feed(&detector, 50, 200));
    KT_CHECK(!sound_detector_hearing(&detector));

    /* And the next sound is a new event. */
    (void)sound_detector_feed(&detector, 600, 2000);
    KT_CHECK(sound_detector_feed(&detector, 600, 2000));
    KT_CHECK_INT_EQ(detector.events, 2);
}

static void test_threshold(void)
{
    struct sound_detector detector;

    ktest_begin("sound: the threshold is what a person moves");

    sound_detector_init(&detector, 1000, 2, 4);

    /* A clap of 600 is under a threshold of 1000. */
    (void)sound_detector_feed(&detector, 600, 2000);
    (void)sound_detector_feed(&detector, 600, 2000);
    KT_CHECK(!sound_detector_hearing(&detector));

    /* The same clap over a threshold of 500 is a sound. */
    sound_detector_set_threshold(&detector, 500);
    (void)sound_detector_feed(&detector, 600, 2000);
    KT_CHECK(sound_detector_feed(&detector, 600, 2000));
    KT_CHECK(sound_detector_hearing(&detector));

    /* The threshold is also the level the band is measured from: half of
     * 500 is 250, and 300 is still a sound. */
    KT_CHECK(!sound_detector_feed(&detector, 300, 900));
    KT_CHECK(sound_detector_hearing(&detector));
}

static void test_edges(void)
{
    struct sound_detector detector;

    ktest_begin("sound: the runs of a detector are at least one window");

    /* A start of 0 and a stop of 0 are read as 1: a detector that answers
     * before it listens is not one. */
    sound_detector_init(&detector, THRESHOLD, 0, 0);
    KT_CHECK_INT_EQ(detector.start_windows, 1);
    KT_CHECK_INT_EQ(detector.stop_windows, 1);
    KT_CHECK(sound_detector_feed(&detector, THRESHOLD, 1000));
    KT_CHECK(sound_detector_hearing(&detector));

    /* A window exactly on the threshold is sound (the comparison is >=). */
    sound_detector_init(&detector, THRESHOLD, 1, 1);
    KT_CHECK(sound_detector_feed(&detector, THRESHOLD, 1000));
    KT_CHECK(sound_detector_hearing(&detector));

    /* The null detector answers nothing rather than crashing. */
    sound_detector_init(NULL, THRESHOLD, 2, 4);
    sound_detector_set_threshold(NULL, THRESHOLD);
    KT_CHECK(!sound_detector_feed(NULL, 4000, 4000));
    KT_CHECK(!sound_detector_hearing(NULL));

    memset(&detector, 0, sizeof(detector));
    sound_detector_init(&detector, THRESHOLD, 2, 4);
    KT_CHECK_INT_EQ(detector.events, 0);
}

void test_sound_detect(void)
{
    test_quiet();
    test_one_window();
    test_band();
    test_threshold();
    test_edges();
}
