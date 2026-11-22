/*
 * Micro OS — the arithmetic of a sound detector.
 *
 * The microphone of the board answers with windows of RMS and peak, and a
 * detector has one question about them: is somebody making a sound, or is the
 * room quiet? The answer cannot be one window of the signal - a door in the
 * corridor, a step, the scrape of a chair and the settling of the filter all
 * look like a clap for one window - so the rule is a number of windows in a
 * row. Sound begins after `start_windows` windows above the threshold and
 * ends after `stop_windows` windows below half of it, and that band between
 * the two levels is what keeps a sound that fades from switching the detector
 * on and off at every window.
 *
 * The thresholds are RMS, not peak: a single sample of a spike raises the peak
 * to the ceiling and says nothing about the room. The detector is fed the
 * numbers of drv_mic_read_level() and knows nothing of a microphone, which is
 * why the host tests can replay any room through it.
 */
#ifndef MICRO_OS_DRIVERS_SOUND_DETECT_H
#define MICRO_OS_DRIVERS_SOUND_DETECT_H

#include <stdbool.h>

/* The largest RMS a 16 bit window can have. */
#define SOUND_RMS_MAX 32767u

struct sound_detector {
    unsigned threshold;     /* RMS at or above which a window is sound */
    unsigned start_windows; /* windows in a row to begin an event */
    unsigned stop_windows;  /* windows in a row below the band to end it */
    unsigned above;         /* the run of windows above the threshold */
    unsigned below;         /* the run of windows below the band */
    bool hearing;           /* the detector hears sound now */
    unsigned events;        /* how many times sound began */
    unsigned last_rms;      /* the last window, as it came in */
    unsigned last_peak;
};

/*
 * A detector with a threshold and the two lengths of its runs. A start of 0
 * and a stop of 0 are read as 1: a detector that needs no window at all
 * would answer before it listened.
 */
void sound_detector_init(struct sound_detector *detector, unsigned threshold,
                         unsigned start_windows, unsigned stop_windows);

/* The threshold of a running detector; a person may move it at any moment. */
void sound_detector_set_threshold(struct sound_detector *detector, unsigned threshold);

/*
 * One window of the signal. Returns true when the window turned the detector
 * from quiet to hearing (an event), false otherwise - including a window of
 * a sound that is still going on.
 */
bool sound_detector_feed(struct sound_detector *detector, unsigned rms, unsigned peak);

/* Whether the detector hears sound after the windows it has been fed. */
bool sound_detector_hearing(const struct sound_detector *detector);

#endif /* MICRO_OS_DRIVERS_SOUND_DETECT_H */
