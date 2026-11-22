/*
 * Micro OS — the arithmetic of a sound detector.
 *
 * See sound_detect.h for what the detector promises and why the two lengths
 * of the runs exist. The band is half of the threshold: a sound that is
 * fading is still a sound until it is well below the level that started it,
 * or the detector would declare the room quiet between two words.
 */
#include "drivers/sound_detect.h"

#include <stddef.h>

void sound_detector_init(struct sound_detector *detector, unsigned threshold,
                         unsigned start_windows, unsigned stop_windows)
{
    if (detector == NULL) {
        return;
    }

    detector->threshold = threshold;
    detector->start_windows = start_windows > 0 ? start_windows : 1;
    detector->stop_windows = stop_windows > 0 ? stop_windows : 1;
    detector->above = 0;
    detector->below = 0;
    detector->hearing = false;
    detector->events = 0;
    detector->last_rms = 0;
    detector->last_peak = 0;
}

void sound_detector_set_threshold(struct sound_detector *detector, unsigned threshold)
{
    if (detector == NULL) {
        return;
    }

    detector->threshold = threshold;
}

bool sound_detector_feed(struct sound_detector *detector, unsigned rms, unsigned peak)
{
    bool began = false;

    if (detector == NULL) {
        return false;
    }

    detector->last_rms = rms;
    detector->last_peak = peak;

    if (rms >= detector->threshold) {
        detector->below = 0;

        if (!detector->hearing) {
            detector->above++;

            if (detector->above >= detector->start_windows) {
                detector->hearing = true;
                detector->events++;
                began = true;
            }
        }

        return began;
    }

    if (detector->hearing) {
        /* The end of an event needs the sound to be well below the level
         * that started it: the band between the threshold and half of it is
         * where a fading word lives, and it is still a word. */
        if (rms * 2u < detector->threshold) {
            detector->below++;

            if (detector->below >= detector->stop_windows) {
                detector->hearing = false;
                detector->above = 0;
                detector->below = 0;
            }
        } else {
            detector->below = 0;
        }

        return false;
    }

    detector->above = 0;

    return false;
}

bool sound_detector_hearing(const struct sound_detector *detector)
{
    return detector != NULL && detector->hearing;
}
