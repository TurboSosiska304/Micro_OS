/*
 * Micro OS — RTC driver (BM8563) of the StickC Plus2.
 *
 * The clock is on the I2C bus at address 0x51. The check reads the time
 * registers and reports the current time, which also proves that the
 * crystal is running (the clock valid bit is set).
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <stdio.h>

#include "kernel/kmod.h"
#include "kshell/kshell.h"

#define BM_REG_SEC 0x00
#define BM_REG_MIN 0x01
#define BM_REG_HOUR 0x02
#define BM_REG_DAY 0x03
#define BM_REG_MONTH 0x04
#define BM_REG_YEAR 0x05

static bool s_present;
static bool s_valid;
static uint8_t s_sec;
static uint8_t s_min;
static uint8_t s_hour;

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)((value >> 4) * 10 + (value & 0x0F));
}

static int drv_rtc_init(void)
{
    uint8_t probe = 0;

    if (drv_i2c_init() != 0) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, &probe, 1) != 0) {
        return -2;
    }

    s_present = true;
    return 0;
}

static int drv_rtc_check(void)
{
    uint8_t raw[3];

    if (!s_present) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) != 0) {
        return -2;
    }

    /* Bit 7 of the seconds register is the "clock integrity" flag. */
    s_valid = (raw[0] & 0x80) != 0;
    s_sec = bcd_to_bin(raw[0] & 0x7F);
    s_min = bcd_to_bin(raw[1] & 0x7F);
    s_hour = bcd_to_bin(raw[2] & 0x7F);

    /*
     * Without a backup battery the clock of the BM8563 does not run, the
     * registers keep random values. The chip itself works, so the module is
     * only degraded, not broken.
     */
    if (s_sec > 59 || s_min > 59 || s_hour > 23) {
        s_valid = false;
    }

    return s_valid ? 0 : 1;
}

static const char *drv_rtc_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%02u:%02u:%02u%s", s_hour, s_min, s_sec,
             s_valid ? "" : " (no clock)");
    return buf;
}

static const struct kmod s_rtc_module = {
    .name = "rtc",
    .desc = "BM8563 real time clock on I2C",
    .init = drv_rtc_init,
    .check = drv_rtc_check,
    .detail = drv_rtc_detail,
};

/* The shell command that shows the time of the real time clock. */
static int cmd_date(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("%02u:%02u:%02u%s\n", s_hour, s_min, s_sec, s_valid ? "" : " (no clock)");

    return KSHELL_OK;
}

int drv_rtc_register(void)
{
    int rc = kmod_register(&s_rtc_module);

    kshell_register("date", "the time of the RTC", cmd_date);

    return rc;
}
