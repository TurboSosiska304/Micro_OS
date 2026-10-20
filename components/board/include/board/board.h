/*
 * Micro OS — board description.
 *
 * Static description of the hardware the kernel is running on. Used by the
 * boot banner, by `uname -a` in the shell and by driver probing.
 */
#ifndef MICRO_OS_BOARD_BOARD_H
#define MICRO_OS_BOARD_BOARD_H

#include "kernel/types.h"

struct board_info {
    const char *name;    /* "stickc_plus2" */
    const char *desc;    /* "M5Stack StickC Plus2" */
    const char *soc;     /* "ESP32-PICO-V3-02" */
    k_u32_t flash_mb;    /* internal flash size */
    k_u32_t psram_kb;    /* external PSRAM size */
    const char *lcd;     /* display description */
    const char *imu;     /* 6-axis IMU */
    const char *rtc;     /* real time clock */
    const char *audio;   /* microphone / audio */
    const char *power;   /* power management note */
    const char *buttons; /* user buttons */
    bool has_buzzer;
    bool has_ir;
    bool has_mic;
};

/* Description of the board this binary was built for. */
const struct board_info *board_get_info(void);

#endif /* MICRO_OS_BOARD_BOARD_H */
