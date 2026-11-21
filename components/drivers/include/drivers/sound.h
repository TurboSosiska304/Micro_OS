/*
 * Micro OS — the sound detector of the board, over the microphone.
 *
 * The microphone (SPM1423, 16 kHz) answers with windows of RMS and peak; the
 * detector of sound_detect.h turns a run of windows into "somebody is making a
 * sound" and back. This file is the part that has a board under it: it reads
 * the windows of drv_mic, offers a bounded listen for the shell and the
 * doctor, and can watch the room in a task of its own.
 *
 * The threshold is RMS and the quiet room of this board reads about 20; the
 * default of 200 is ten times that, so a door in the corridor does not start
 * an event and a clap next to the microphone does. A person who knows their
 * room better moves it with `sound threshold`.
 *
 * The detector is not a wake source: the microphone is powered down in deep
 * sleep on this board, and the wearable of P3 is a timer and a clock, not an
 * always listening ear.
 */
#ifndef MICRO_OS_DRIVERS_SOUND_H
#define MICRO_OS_DRIVERS_SOUND_H

#include "drivers/mic.h"
#include "kernel/types.h"

/* The RMS that counts as sound by default, and the window the detector
 * reads. The runs of the detector are two windows to begin an event and four
 * to end it: 100 ms of sound begins it, 200 ms of quiet ends it. */
#define DRV_SOUND_THRESHOLD 200u
#define DRV_SOUND_WINDOW_MS 50u
#define DRV_SOUND_START_WINDOWS 2u
#define DRV_SOUND_STOP_WINDOWS 4u

/* What one bounded listen of the shell heard. */
struct drv_sound_result {
    unsigned windows;   /* windows of the microphone read */
    unsigned events;    /* how many events the detector began */
    unsigned rms;       /* the loudest window of the listen */
    unsigned peak;      /* its peak */
    unsigned threshold; /* the threshold the listen used */
};

/*
 * Listen to the microphone for `ms` and count the events of the detector.
 * The call blocks the task for the length of the listen; it is the shape the
 * shell wants and the doctor can afford. A watcher that runs at the same time
 * reads the same stream, so the two share the windows of the microphone and
 * neither starves: the microphone itself is one channel and serialises its
 * readers. Returns 0 and fills *result, or a negative error of
 * drv_mic_read_level() when the microphone does not answer.
 */
int drv_sound_listen(unsigned ms, struct drv_sound_result *result);

/* The threshold of the watcher and of the next listen, and its setter. A
 * threshold of 0 is refused with -1. */
unsigned drv_sound_threshold(void);
int drv_sound_set_threshold(unsigned threshold);

/*
 * Watch the room in a task of its own: every event is a line of the log. One
 * watcher at a time; starting a second answers -1, and -2 is the failure of
 * the task. The stop waits for the task to end (at most a second) and answers
 * -1 when no watcher runs, -2 when it did not end in time.
 */
int drv_sound_watch_start(void);
int drv_sound_watch_stop(void);

/* The state of the watcher, for the command that shows it. */
bool drv_sound_watching(void);
bool drv_sound_hearing(void);
unsigned drv_sound_events(void);
unsigned drv_sound_last_rms(void);
unsigned drv_sound_last_peak(void);

/* Register the module of the hardware doctor and the `sound` command. */
int drv_sound_register(void);

#endif /* MICRO_OS_DRIVERS_SOUND_H */
