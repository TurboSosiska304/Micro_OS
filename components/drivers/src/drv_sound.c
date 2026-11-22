/*
 * Micro OS — the sound detector of the board, over the microphone.
 *
 * See sound.h for the promise and sound_detect.h for the arithmetic. The
 * file is compiled only with CONFIG_MICRO_OS_MIC: a build without the
 * microphone registers the module with nothing to check, so the doctor says
 * "skip" instead of calling a dead channel a fault.
 */
#include "drivers/internal.h"
#include "drivers/sound.h"
#include "drivers/sound_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_MICRO_OS_MIC

#include "kernel/kmod.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

/* The first window of a started channel is the settling of its filter: the
 * doctor drops one before it reads the room. */
#define SOUND_SETTLE_MS 100

/* The longest listen the command accepts: a sanity bound, not a limit. */
#define SOUND_LISTEN_MAX_MS 60000u

static k_mutex_t s_lock = K_MUTEX_INITIALIZER;
static struct sound_detector s_detector;
static unsigned s_threshold = DRV_SOUND_THRESHOLD;
static volatile bool s_watch;
static struct task *s_watch_task;
static char s_detail[KMOD_INFO_MAX];

static void sound_state_init(struct sound_detector *detector, unsigned threshold)
{
    sound_detector_init(detector, threshold, DRV_SOUND_START_WINDOWS, DRV_SOUND_STOP_WINDOWS);
}

/*
 * One window of the microphone into the detector. The lock is held for the
 * feed only: the read itself is the long part and two readers of the I2S
 * channel are serialised by the microphone's own lock.
 */
static int sound_feed(struct sound_detector *detector, bool *began)
{
    struct drv_mic_level level;
    int rc = drv_mic_read_level(&level, DRV_SOUND_WINDOW_MS);

    if (rc != 0) {
        return rc;
    }

    if (k_mutex_lock(&s_lock) == 0) {
        *began = sound_detector_feed(detector, level.rms, level.peak);
        k_mutex_unlock(&s_lock);
    } else {
        return -1;
    }

    return 0;
}

int drv_sound_listen(unsigned ms, struct drv_sound_result *result)
{
    struct sound_detector detector;
    unsigned window_ms = DRV_SOUND_WINDOW_MS;
    unsigned windows = 0;
    unsigned loudest = 0;
    unsigned peak = 0;
    unsigned elapsed = 0;
    int rc = 0;

    if (result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));

    ms = ms < window_ms ? window_ms : K_MIN(ms, SOUND_LISTEN_MAX_MS);
    sound_state_init(&detector, drv_sound_threshold());

    while (elapsed < ms) {
        bool began = false;

        rc = sound_feed(&detector, &began);
        if (rc != 0) {
            return rc;
        }

        windows++;
        elapsed += window_ms;

        if (detector.last_rms > loudest) {
            loudest = detector.last_rms;
            peak = detector.last_peak;
        }
    }

    result->windows = windows;
    result->events = detector.events;
    result->rms = loudest;
    result->peak = peak;
    result->threshold = detector.threshold;

    return 0;
}

unsigned drv_sound_threshold(void)
{
    return s_threshold;
}

int drv_sound_set_threshold(unsigned threshold)
{
    if (threshold == 0 || threshold > SOUND_RMS_MAX) {
        return -1;
    }

    s_threshold = threshold;

    if (k_mutex_lock(&s_lock) == 0) {
        sound_detector_set_threshold(&s_detector, threshold);
        k_mutex_unlock(&s_lock);
    }

    return 0;
}

bool drv_sound_hearing(void)
{
    bool hearing = false;

    if (k_mutex_lock(&s_lock) == 0) {
        hearing = sound_detector_hearing(&s_detector);
        k_mutex_unlock(&s_lock);
    }

    return hearing;
}

unsigned drv_sound_events(void)
{
    unsigned events = 0;

    if (k_mutex_lock(&s_lock) == 0) {
        events = s_detector.events;
        k_mutex_unlock(&s_lock);
    }

    return events;
}

unsigned drv_sound_last_rms(void)
{
    unsigned rms = 0;

    if (k_mutex_lock(&s_lock) == 0) {
        rms = s_detector.last_rms;
        k_mutex_unlock(&s_lock);
    }

    return rms;
}

unsigned drv_sound_last_peak(void)
{
    unsigned peak = 0;

    if (k_mutex_lock(&s_lock) == 0) {
        peak = s_detector.last_peak;
        k_mutex_unlock(&s_lock);
    }

    return peak;
}

bool drv_sound_watching(void)
{
    return s_watch_task != NULL;
}

/* The task of the watcher: a window, a feed, a line of the log on an event. */
static void sound_task(void *arg)
{
    K_UNUSED(arg);

    while (s_watch) {
        struct drv_mic_level window;
        bool began = false;

        /* The read of the microphone is the wait of the loop; a room with no
         * data still yields the task and tries again. */
        if (drv_mic_read_level(&window, DRV_SOUND_WINDOW_MS) != 0) {
            k_sched_sleep_ms(DRV_SOUND_WINDOW_MS);
            continue;
        }

        if (k_mutex_lock(&s_lock) == 0) {
            began = sound_detector_feed(&s_detector, window.rms, window.peak);
            k_mutex_unlock(&s_lock);
        }

        if (began) {
            kinfo("sound: heard rms %u, peak %u (threshold %u)", window.rms, window.peak,
                  drv_sound_threshold());
        }
    }

    s_watch_task = NULL;
    task_exit(0);
}

int drv_sound_watch_start(void)
{
    if (s_watch_task != NULL) {
        return -1;
    }

    /* A fresh detector for a fresh watch: the events of the last one are not
     * the events of this one. */
    if (k_mutex_lock(&s_lock) == 0) {
        sound_state_init(&s_detector, s_threshold);
        k_mutex_unlock(&s_lock);
    }

    s_watch = true;
    s_watch_task = task_create("sound", sound_task, NULL, TASK_PRIO_DEFAULT);

    if (s_watch_task == NULL) {
        s_watch = false;
        return -2;
    }

    kinfo("sound: watching the room, threshold %u", drv_sound_threshold());

    return 0;
}

int drv_sound_watch_stop(void)
{
    unsigned waited = 0;

    if (s_watch_task == NULL) {
        return -1;
    }

    s_watch = false;

    /* The task notices at the end of the window it is in; the wait is the
     * length of one window and a little more. */
    while (s_watch_task != NULL && waited < 1000) {
        k_sched_sleep_ms(DRV_SOUND_WINDOW_MS);
        waited += DRV_SOUND_WINDOW_MS;
    }

    if (s_watch_task != NULL) {
        return -2;
    }

    /*
     * Nobody feeds the detector now, so its last state would be read as the
     * state of a room nobody is listening to: the room is quiet for the
     * purpose of the next command, and the events of the watch stay counted
     * until the next one starts.
     */
    if (k_mutex_lock(&s_lock) == 0) {
        s_detector.hearing = false;
        s_detector.above = 0;
        s_detector.below = 0;
        k_mutex_unlock(&s_lock);
    }

    return 0;
}

/* --- the doctor --- */

/*
 * The check listens to a settled microphone. A detector that begins an event
 * on a quiet window is the fault this module guards against; a room that is
 * loud at the boot is not a fault but it is worth the numbers, so it is
 * reported as degraded with the level that did it.
 */
static int drv_sound_check(void)
{
    struct sound_detector detector;
    struct drv_mic_level level;
    int rc;

    /* The settling of the filter goes first, as in the microphone's own
     * check. */
    rc = drv_mic_read_level(&level, SOUND_SETTLE_MS);
    if (rc != 0) {
        snprintf(s_detail, sizeof(s_detail), "the microphone does not answer (%d)", rc);
        return 1;
    }

    sound_state_init(&detector, drv_sound_threshold());

    /* Two windows in a row, as the detector itself asks: one window of a
     * spike is not a sound. */
    for (int i = 0; i < (int)DRV_SOUND_START_WINDOWS; i++) {
        rc = drv_mic_read_level(&level, DRV_SOUND_WINDOW_MS);
        if (rc != 0) {
            snprintf(s_detail, sizeof(s_detail), "the microphone does not answer (%d)", rc);
            return 1;
        }

        (void)sound_detector_feed(&detector, level.rms, level.peak);
    }

    if (sound_detector_hearing(&detector)) {
        snprintf(s_detail, sizeof(s_detail), "the room is loud: rms %u >= %u", level.rms,
                 detector.threshold);
        return 1;
    }

    snprintf(s_detail, sizeof(s_detail), "quiet: rms %u, peak %u, threshold %u", level.rms,
             level.peak, detector.threshold);

    return 0;
}

static const char *drv_sound_detail(void)
{
    return s_detail;
}

static const struct kmod s_sound_module = {
    .name = "sound",
    .desc = "sound detector over the microphone",
    .init = NULL,
    .check = drv_sound_check,
    .detail = drv_sound_detail,
};

/* --- the shell --- */

static void sound_usage(void)
{
    kshell_printf("usage: sound | sound listen [ms] | sound threshold <rms>\n");
    kshell_printf("       sound watch on|off\n");
}

static void sound_show(void)
{
    kshell_printf("sound: threshold %u, %s, %u event(s), last rms %u, peak %u\n",
                  drv_sound_threshold(), drv_sound_hearing() ? "hearing" : "quiet",
                  drv_sound_events(), drv_sound_last_rms(), drv_sound_last_peak());
    kshell_printf("sound: watching %s\n", drv_sound_watching() ? "on" : "off");
}

static int sound_listen_cmd(int argc, char **argv)
{
    struct drv_sound_result result;
    unsigned ms = 3000;
    int rc;

    if (argc > 2) {
        char *end = NULL;

        ms = (unsigned)strtoul(argv[2], &end, 10);
        if (argv[2][0] == '\0' || end == NULL || *end != '\0' || ms == 0) {
            kshell_printf("sound listen: no such number of milliseconds: %s\n", argv[2]);
            return KSHELL_ERROR;
        }
    }

    rc = drv_sound_listen(ms, &result);

    if (rc != 0) {
        kshell_printf("sound listen: the microphone does not answer (%d)\n", rc);
        return KSHELL_ERROR;
    }

    kshell_printf("sound: %u ms, %u window(s), %u event(s), loudest rms %u, peak %u\n", ms,
                  result.windows, result.events, result.rms, result.peak);

    if (result.events == 0) {
        kshell_printf("sound: quiet, nothing above the threshold %u\n", result.threshold);
    }

    return KSHELL_OK;
}

static int sound_threshold_cmd(int argc, char **argv)
{
    char *end = NULL;
    unsigned long threshold;

    if (argc != 3) {
        sound_usage();
        return KSHELL_USAGE;
    }

    threshold = strtoul(argv[2], &end, 10);
    if (argv[2][0] == '\0' || end == NULL || *end != '\0' || threshold == 0 ||
        threshold > SOUND_RMS_MAX) {
        kshell_printf("sound threshold: 1..%u is the scale of the rms\n", SOUND_RMS_MAX);
        return KSHELL_ERROR;
    }

    if (drv_sound_set_threshold((unsigned)threshold) != 0) {
        kshell_printf("sound threshold: the threshold is not taken\n");
        return KSHELL_ERROR;
    }

    kshell_printf("sound: threshold %lu\n", threshold);

    return KSHELL_OK;
}

static int sound_watch_cmd(int argc, char **argv)
{
    int rc;

    if (argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        sound_usage();
        return KSHELL_USAGE;
    }

    if (strcmp(argv[2], "on") == 0) {
        rc = drv_sound_watch_start();
        if (rc == -1) {
            kshell_printf("sound watch: a watcher is already running\n");
            return KSHELL_ERROR;
        }
        if (rc != 0) {
            kshell_printf("sound watch: the task cannot be started\n");
            return KSHELL_ERROR;
        }

        kshell_printf("sound: watching, threshold %u; every event is a line of the log\n",
                      drv_sound_threshold());
        return KSHELL_OK;
    }

    rc = drv_sound_watch_stop();
    if (rc == -1) {
        kshell_printf("sound watch: no watcher is running\n");
        return KSHELL_ERROR;
    }
    if (rc != 0) {
        kshell_printf("sound watch: the watcher did not end\n");
        return KSHELL_ERROR;
    }

    kshell_printf("sound: the watcher is off, %u event(s) heard\n", drv_sound_events());

    return KSHELL_OK;
}

static int cmd_sound(int argc, char **argv)
{
    if (argc == 1) {
        sound_show();
        return KSHELL_OK;
    }

    if (strcmp(argv[1], "listen") == 0) {
        return sound_listen_cmd(argc, argv);
    }

    if (strcmp(argv[1], "threshold") == 0) {
        return sound_threshold_cmd(argc, argv);
    }

    if (strcmp(argv[1], "watch") == 0) {
        return sound_watch_cmd(argc, argv);
    }

    sound_usage();
    return KSHELL_USAGE;
}

int drv_sound_register(void)
{
    sound_state_init(&s_detector, s_threshold);

    kshell_register("sound", "the sound detector: sound[, listen, threshold, watch]", cmd_sound);

    return kmod_register(&s_sound_module);
}

#else /* !CONFIG_MICRO_OS_MIC */

/*
 * The microphone is not in this build, so there is nothing to listen to: the
 * module is registered with no check and the doctor says "skip".
 */
int drv_sound_listen(unsigned ms, struct drv_sound_result *result)
{
    (void)ms;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    return -2;
}

unsigned drv_sound_threshold(void)
{
    return DRV_SOUND_THRESHOLD;
}

int drv_sound_set_threshold(unsigned threshold)
{
    (void)threshold;
    return -1;
}

int drv_sound_watch_start(void)
{
    return -2;
}

int drv_sound_watch_stop(void)
{
    return -1;
}

bool drv_sound_watching(void)
{
    return false;
}

bool drv_sound_hearing(void)
{
    return false;
}

unsigned drv_sound_events(void)
{
    return 0;
}

unsigned drv_sound_last_rms(void)
{
    return 0;
}

unsigned drv_sound_last_peak(void)
{
    return 0;
}

static const struct kmod s_sound_module = {
    .name = "sound",
    .desc = "sound detector over the microphone, not in this build",
    .init = NULL,
    .check = NULL,
    .detail = NULL,
};

int drv_sound_register(void)
{
    return kmod_register(&s_sound_module);
}

#endif /* CONFIG_MICRO_OS_MIC */
