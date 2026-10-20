#include "board/board.h"

#if defined(KERNEL_BOARD_STICKC_PLUS2)

static const struct board_info g_info = {
    .name = "stickc_plus2",
    .desc = "M5Stack StickC Plus2",
    .soc = "ESP32-PICO-V3-02",
    .flash_mb = 8,
    .psram_kb = 2048,
    .lcd = "1.14\" TFT 135x240 (ST7789v2)",
    .imu = "MPU6886 6-axis",
    .rtc = "BM8563",
    .audio = "SPM1423 MEMS microphone",
    .power = "200 mAh LiPo, no PMIC (HOLD on G4)",
    .buttons = "A (G37), B (G39), C/power (G35)",
    .has_buzzer = true,
    .has_ir = true,
    .has_mic = true,
};

#elif defined(KERNEL_BOARD_SIM)

static const struct board_info g_info = {
    .name = "host_sim",
    .desc = "Micro OS host simulator",
    .soc = "native",
    .flash_mb = 0,
    .psram_kb = 0,
    .lcd = "stdout console",
    .imu = "none",
    .rtc = "host clock",
    .audio = "none",
    .power = "host power supply",
    .buttons = "stdin",
    .has_buzzer = false,
    .has_ir = false,
    .has_mic = false,
};

#else

static const struct board_info g_info = {
    .name = "unknown",
    .desc = "unknown board",
    .soc = "unknown",
    .flash_mb = 0,
    .psram_kb = 0,
    .lcd = "none",
    .imu = "none",
    .rtc = "none",
    .audio = "none",
    .power = "unknown",
    .buttons = "none",
    .has_buzzer = false,
    .has_ir = false,
    .has_mic = false,
};

#endif

const struct board_info *board_get_info(void)
{
    return &g_info;
}
