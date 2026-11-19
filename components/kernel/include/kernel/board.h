/*
 * Micro OS — the board, as the kernel names it.
 *
 * The boot banner, `uname` and `mfetch` want to say what machine they run
 * on, and the description itself belongs to the platform: the board
 * component knows the pins and the chips. The platform registers the
 * description at boot, and everything above the port reads it from here,
 * so a component can describe the board without depending on the board
 * being in the same repository.
 */
#ifndef MICRO_OS_KERNEL_BOARD_H
#define MICRO_OS_KERNEL_BOARD_H

#include "kernel/types.h"

struct kernel_board {
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

/*
 * The description is registered on the boot of the kernel and lives as
 * long as the platform's copy of it: the kernel only keeps the pointer.
 */
void k_board_register(const struct kernel_board *board);

/* The description of the board, NULL until the boot registers it. */
const struct kernel_board *k_board_get(void);

#endif /* MICRO_OS_KERNEL_BOARD_H */
