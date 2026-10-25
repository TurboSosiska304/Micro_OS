/*
 * Micro OS — RTC driver (BM8563) of the StickC Plus2.
 *
 * The clock is on the I2C bus at address 0x51. The check reads the
 * seven registers of the time in one transaction and lets the core of
 * rtc_time.c decide whether they can be believed: the LV flag of the
 * chip and the plain range of every field. A board whose backup
 * battery went away keeps no time between the power cycles, and the
 * doctor says so out loud — with the date and the time it found —
 * instead of showing a plausible lie.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"
#include "drivers/rtc_time.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kshell/kshell.h"

/*
 * The registers of the chip: two of control and status, and the time
 * after them. The map is the one of the PCF8563 family, which the
 * BM8563 belongs to — reading the time from 0x00 (as the code did
 * once) reads the control registers instead, and the seconds of the
 * clock come out as "hours" like 26:00:00.
 */
#define BM_REG_CTRL1 0x00 /* bit 5 stops the oscillator */
#define BM_REG_CTRL2 0x01 /* the flags of the alarm and the timer, and their enables */
#define BM_REG_SEC 0x02   /* seconds, bit 7 = LV */
#define BM_REG_MIN 0x03
#define BM_REG_HOUR 0x04
#define BM_REG_DAY 0x05
#define BM_REG_WEEKDAY 0x06
#define BM_REG_MONTH 0x07 /* bit 7 = century: 1 = 19xx */
#define BM_REG_YEAR 0x08

static bool s_present;
static struct rtc_datetime s_now;

/* The moment as one line, with the verdict of the doctor behind it. */
static const char *rtc_line(char *buf, size_t size)
{
    char when[24];

    rtc_format(&s_now, when, sizeof(when));
    snprintf(buf, size, "%s%s", when, s_now.valid ? "" : " (no clock)");

    return buf;
}

static int drv_rtc_init(void)
{
    uint8_t probe = 0;
    uint8_t control[2] = {0x00, 0x00};

    if (drv_i2c_init() != 0) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, &probe, 1) != 0) {
        return -2;
    }

    /*
     * The control registers are put into their plain state: a random
     * bit left by a dead battery can keep the oscillator stopped or an
     * interrupt enabled, and the flags of that day stay until somebody
     * clears them. A failure to write them is not fatal — the clock
     * reads the time either way — so it is not counted against the
     * module.
     */
    (void)drv_i2c_write(I2C_BM8563_ADDR, BM_REG_CTRL1, control, sizeof(control));

    s_present = true;
    return 0;
}

static int drv_rtc_check(void)
{
    uint8_t raw[7];

    if (!s_present) {
        return -1;
    }

    if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) != 0) {
        return -2;
    }

    rtc_datetime_decode(raw, &s_now);

    return s_now.valid ? 0 : 1;
}

static const char *drv_rtc_detail(void)
{
    static char buf[48];

    /* The report of the doctor stays one line of time, as it was. */
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s", s_now.hour, s_now.minute, s_now.second,
             s_now.valid ? "" : " (no clock)");

    return buf;
}

static const struct kmod s_rtc_module = {
    .name = "rtc",
    .desc = "BM8563 real time clock on I2C",
    .init = drv_rtc_init,
    .check = drv_rtc_check,
    .detail = drv_rtc_detail,
};

/*
 * The shell: `date` shows the moment the chip holds, and
 * `date set <YYYY-MM-DD> <HH:MM:SS>` writes a new one. The write goes
 * into the seven registers in one transaction; what is printed after
 * it is read back from the chip, so the line is what the clock really
 * holds and not what was asked of it.
 */
static int cmd_date(int argc, char **argv)
{
    char buf[48];

    if (argc == 1) {
        kshell_printf("%s\n", rtc_line(buf, sizeof(buf)));
        return KSHELL_OK;
    }

    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        struct rtc_datetime dt;
        uint8_t raw[7];

        memset(&dt, 0, sizeof(dt));

        if (!rtc_parse_date(argv[2], &dt) || !rtc_parse_time(argv[3], &dt)) {
            kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
            return KSHELL_USAGE;
        }

        if (!rtc_datetime_in_range(&dt)) {
            kshell_printf("date set: no such day: %s %s\n", argv[2], argv[3]);
            return KSHELL_ERROR;
        }

        if (!s_present) {
            kshell_printf("date set: there is no clock to set\n");
            return KSHELL_ERROR;
        }

        rtc_datetime_encode(&dt, raw);

        if (drv_i2c_write(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) != 0) {
            kshell_printf("date set: the clock did not take the time\n");
            return KSHELL_ERROR;
        }

        if (drv_i2c_read(I2C_BM8563_ADDR, BM_REG_SEC, raw, sizeof(raw)) == 0) {
            rtc_datetime_decode(raw, &s_now);
        }

        kshell_printf("%s\n", rtc_line(buf, sizeof(buf)));
        return KSHELL_OK;
    }

    kshell_printf("usage: date | date set <YYYY-MM-DD> <HH:MM:SS>\n");
    return KSHELL_USAGE;
}

int drv_rtc_register(void)
{
    int rc = kmod_register(&s_rtc_module);

    kshell_register("date", "the time of the RTC", cmd_date);

    return rc;
}
