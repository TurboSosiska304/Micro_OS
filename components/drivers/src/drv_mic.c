/*
 * Micro OS — microphone of the StickC Plus2 (SPM1423).
 *
 * The microphone is a MEMS element with a PDM output: the ESP32 generates
 * the clock on G0 and samples the data line G34 with the I2S peripheral in
 * PDM RX mode. There is no register that says "I am here", so the module is
 * checked by listening: a window of the signal is read and its RMS and peak
 * are the answer. A window of digital silence is reported as degraded, not
 * as working, because a microphone that hears nothing is not a microphone.
 *
 * The driver is compiled in by CONFIG_MICRO_OS_MIC. Without it the module is
 * still registered, so the doctor says "skip" and a person knows that the
 * microphone was left out of the build instead of looking for a fault.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"
#include "drivers/mic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_MICRO_OS_MIC

#include "driver/i2s_pdm.h"
#include "esp_timer.h"
#include "kernel/kmod.h"
#include "kernel/lock.h"
#include "kshell/kshell.h"

/*
 * 16 kHz is the rate the PDM filter of the ESP32 and the element agree on,
 * and the one M5Stack uses for the microphone of this board. The window of
 * the check is short enough to keep the boot quick and long enough for a
 * spoken word to land in it.
 */
#define MIC_SAMPLE_RATE 16000
#define MIC_CHECK_WINDOW_MS 50
#define MIC_WINDOW_MIN_MS 10
#define MIC_WINDOW_MAX_MS 500

/* A read may wait for a whole window plus this, never for ever. */
#define MIC_READ_TIMEOUT_MS 250

/*
 * The first window of a started channel is the settling of the input filter,
 * and a channel nobody reads keeps its last samples in the DMA ring: the
 * check drops one window of this length before it measures, so that the
 * number it reports is the room and not the moment the clock started.
 */
#define MIC_FLUSH_WINDOW_MS 100

/*
 * Below these the line is quiet. A working element in a quiet room reads
 * about 4 and 16 here; a line that does not answer at all is a constant and
 * reads zero.
 */
#define MIC_QUIET_RMS 2
#define MIC_QUIET_PEAK 8

/* One read of the DMA: 256 samples is 16 ms at 16 kHz, 512 bytes of RAM. */
#define MIC_CHUNK_SAMPLES 256

static i2s_chan_handle_t s_rx;
static k_mutex_t s_lock = K_MUTEX_INITIALIZER;
static bool s_ready;
static bool s_heard;
static char s_detail[KMOD_INFO_MAX];

/* Integer square root by Newton, so the driver needs no libm. */
static unsigned mic_isqrt(k_u64_t value)
{
    k_u64_t x = value;
    k_u64_t y = (x + 1) / 2;

    if (value == 0) {
        return 0;
    }

    while (y < x) {
        x = y;
        y = (x + value / x) / 2;
    }

    return (unsigned)x;
}

int drv_mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = PIN_MIC_CLK,
                .din = PIN_MIC_DATA,
            },
    };

    if (s_ready) {
        return 0;
    }

    if (i2s_new_channel(&chan_cfg, NULL, &s_rx) != ESP_OK) {
        return -1;
    }

    if (i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg) != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return -2;
    }

    if (i2s_channel_enable(s_rx) != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return -3;
    }

    s_ready = true;
    return 0;
}

int drv_mic_read_level(struct drv_mic_level *level, unsigned window_ms)
{
    static int16_t chunk[MIC_CHUNK_SAMPLES];
    k_i64_t sum = 0;
    k_u64_t sum_sq = 0;
    unsigned samples = 0;
    k_i32_t lowest = 0;
    k_i32_t highest = 0;
    unsigned target;
    int64_t deadline;
    int rc = 0;

    if (level == NULL) {
        return -1;
    }

    memset(level, 0, sizeof(*level));

    if (!s_ready) {
        return -2;
    }

    /* A caller may ask for any window; the driver keeps it sane. */
    window_ms = K_MAX(window_ms, MIC_WINDOW_MIN_MS);
    window_ms = K_MIN(window_ms, MIC_WINDOW_MAX_MS);
    target = (MIC_SAMPLE_RATE / 1000) * window_ms;

    /*
     * The microphone is one stream: two readers would steal samples from
     * each other, so the window is read by one task at a time.
     */
    if (k_mutex_lock(&s_lock) != 0) {
        return -3;
    }

    deadline = esp_timer_get_time() + (int64_t)window_ms * 1000;

    while (samples < target) {
        size_t want = K_MIN((size_t)(target - samples) * sizeof(int16_t), sizeof(chunk));
        size_t got = 0;
        size_t i;

        if (i2s_channel_read(s_rx, chunk, want, &got, MIC_READ_TIMEOUT_MS) != ESP_OK) {
            rc = -3;
            break;
        }

        for (i = 0; i < got / sizeof(int16_t); i++) {
            k_i32_t value = chunk[i];

            sum += value;
            sum_sq += (k_u64_t)(value * value);
            samples++;

            if (samples == 1) {
                lowest = value;
                highest = value;
            } else {
                lowest = K_MIN(lowest, value);
                highest = K_MAX(highest, value);
            }
        }

        /* The window is a budget of time, not a promise of a full buffer. */
        if (esp_timer_get_time() >= deadline) {
            break;
        }
    }

    k_mutex_unlock(&s_lock);

    if (rc != 0) {
        return rc;
    }

    if (samples == 0) {
        return -4;
    }

    {
        /*
         * The converter answers around an offset of its own (about 1500
         * counts on this board), which says nothing about the sound. The
         * level is the signal around that offset: the average is taken out
         * of both numbers, and the peak is the distance to the farthest
         * sample.
         *
         * The variance is taken from the sums and not from a rounded
         * average: around an offset of 1500 counts one count of rounding
         * would add some forty counts of a level that is not there.
         */
        k_i64_t n = (k_i64_t)samples;
        k_i64_t mean = sum / n;
        k_i64_t power = (k_i64_t)sum_sq * n - sum * sum;
        k_i32_t up = (k_i32_t)(highest - mean);
        k_i32_t down = (k_i32_t)(mean - lowest);

        level->samples = samples;
        level->rms = mic_isqrt(power > 0 ? (k_u64_t)(power / (n * n)) : 0);
        level->peak = (unsigned)(up > down ? up : down);
    }

    return 0;
}

bool drv_mic_present(void)
{
    return s_ready && s_heard;
}

static int drv_mic_check(void)
{
    struct drv_mic_level level;
    int rc;

    if (!s_ready) {
        snprintf(s_detail, sizeof(s_detail), "the i2s channel is not started");
        return -1;
    }

    /* The settling of the filter and the stale ring go first. */
    (void)drv_mic_read_level(&level, MIC_FLUSH_WINDOW_MS);

    rc = drv_mic_read_level(&level, MIC_CHECK_WINDOW_MS);

    if (rc == -4) {
        /* The channel runs, the line says nothing: a quiet or dead microphone. */
        snprintf(s_detail, sizeof(s_detail), "no data in %u ms", MIC_CHECK_WINDOW_MS);
        return 1;
    }

    if (rc != 0) {
        snprintf(s_detail, sizeof(s_detail), "i2s read failed (%d)", rc);
        return -1;
    }

    s_heard = true;

    if (level.rms < MIC_QUIET_RMS && level.peak < MIC_QUIET_PEAK) {
        snprintf(s_detail, sizeof(s_detail), "silence: rms %u, peak %u", level.rms, level.peak);
        return 1;
    }

    snprintf(s_detail, sizeof(s_detail), "%u Hz, rms %u, peak %u", MIC_SAMPLE_RATE, level.rms,
             level.peak);
    return 0;
}

static const char *drv_mic_detail(void)
{
    return s_detail;
}

/* --- the shell --- */

/* The reason a window did not arrive, in words a person can act on. */
static const char *mic_error_text(int rc)
{
    switch (rc) {
    case -1:
        return "the size of the window is not usable";
    case -2:
        return "the i2s channel is not started";
    case -3:
        return "the i2s read failed";
    case -4:
        return "no data came from the microphone";
    default:
        return "the microphone is not working";
    }
}

/*
 * The command "mic": one window of the microphone and the level in it, so
 * that a person can say a word and see the numbers move. The argument is
 * the length of the window in milliseconds.
 */
static int cmd_mic(int argc, char **argv)
{
    struct drv_mic_level level;
    unsigned window = 200;
    int rc;

    if (argc > 1) {
        window = (unsigned)strtoul(argv[1], NULL, 10);
    }

    rc = drv_mic_read_level(&level, window);

    if (rc != 0) {
        kshell_printf("mic: %s\n", mic_error_text(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("mic: %u ms, rms %u, peak %u, %u samples\n", window, level.rms, level.peak,
                  level.samples);

    if (level.rms < MIC_QUIET_RMS && level.peak < MIC_QUIET_PEAK) {
        kshell_printf("mic: silence - the microphone hears nothing\n");
    }

    return KSHELL_OK;
}

static const struct kmod s_mic_module = {
    .name = "mic",
    .desc = "SPM1423 MEMS microphone on I2S (PDM)",
    .init = drv_mic_init,
    .check = drv_mic_check,
    .detail = drv_mic_detail,
};

int drv_mic_register(void)
{
    kshell_register("mic", "the microphone: mic [ms]", cmd_mic);

    return kmod_register(&s_mic_module);
}

#else /* !CONFIG_MICRO_OS_MIC */

/*
 * The driver is left out of the build: the module is still registered so
 * that the doctor report names it, and the doctor marks it as skipped.
 */
int drv_mic_init(void)
{
    return -1;
}

int drv_mic_read_level(struct drv_mic_level *level, unsigned window_ms)
{
    (void)window_ms;

    if (level != NULL) {
        memset(level, 0, sizeof(*level));
    }

    return -2;
}

bool drv_mic_present(void)
{
    return false;
}

static const struct kmod s_mic_module = {
    .name = "mic",
    .desc = "SPM1423 MEMS microphone on I2S (PDM), not in this build",
    .init = NULL,
    .check = NULL,
    .detail = NULL,
};

int drv_mic_register(void)
{
    return kmod_register(&s_mic_module);
}

#endif /* CONFIG_MICRO_OS_MIC */
