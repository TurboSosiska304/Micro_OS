/*
 * Micro OS — the microphone of the board.
 *
 * The SPM1423 is a MEMS microphone that answers with a stream of bits and
 * has no register to read: the only proof that it works is a window of
 * signal. The driver turns such a window into two numbers, the RMS and the
 * peak, and that is what a program and the hardware doctor look at.
 */
#ifndef MICRO_OS_DRIVERS_MIC_H
#define MICRO_OS_DRIVERS_MIC_H

#include "kernel/types.h"

/* What one window of the signal looks like. */
struct drv_mic_level {
    unsigned rms;     /* root mean square around the average, 0..32767 */
    unsigned peak;    /* farthest sample from the average, 0..32767 */
    unsigned samples; /* how many samples the window actually holds */
};

/*
 * Start the I2S channel of the microphone. Called by the module registry at
 * the boot and safe to call again: a started channel is not started twice.
 * Returns 0, or a negative number when the peripheral cannot be set up.
 */
int drv_mic_init(void);

/*
 * Read the level of the signal over a window of window_ms milliseconds.
 *
 * The channel runs from the boot, so the window is the audio of the moment
 * or of the few tens of milliseconds before it: the DMA buffer keeps the
 * last samples while nobody reads them. The call waits for the samples and
 * never longer than the window plus a timeout, so it is safe on the init
 * task. Returns 0 and fills *level on a window with samples in it, -1 for a
 * bad argument, -2 when the channel is not started, -3 when the window
 * cannot be read and -4 when the line delivered nothing at all.
 */
int drv_mic_read_level(struct drv_mic_level *level, unsigned window_ms);

/* Whether the microphone answered at least once with a window of samples. */
bool drv_mic_present(void);

#endif /* MICRO_OS_DRIVERS_MIC_H */
