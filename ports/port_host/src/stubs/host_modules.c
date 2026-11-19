/*
 * Micro OS — the modules of the hardware doctor on the host.
 *
 * The board registers a module per piece of hardware, and the report of the
 * doctor is a part of the system a person reads after every boot. The
 * simulator has no chip, but it must tell the same story in the same names:
 * `modules` then prints a report of the host instead of an empty table, and
 * the difference between "works", "works with a limitation" and "there is
 * nothing here" is visible on a laptop as it is on the board.
 *
 * The states are honest: a laptop has no radio and no IMU, so those modules
 * are degraded or skipped rather than reported as working hardware.
 */
#include "host_stubs.h"
#include "port_host.h"
#include "port_host_psram.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/kmod.h"
#include "kernel/sysinfo.h"

/* --- memory --- */

static k_size_t s_psram_size;
static k_size_t s_flash_size;

/*
 * The pool of the host is made on the first allocation, so the report of
 * the memory would say "0 KB free" until something happened to allocate:
 * the module of the doctor makes it now, and the number a person reads is
 * the number the kernel has.
 */
static int psram_module_init(void)
{
    host_psram_init();

    s_psram_size = k_sysinfo_psram_total();
    return s_psram_size > 0 ? 0 : -1;
}

static int host_psram_check(void)
{
    return s_psram_size > 0 ? 0 : -1;
}

static const char *host_psram_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(s_psram_size / 1024));
    return buf;
}

static int host_flash_init(void)
{
    struct stat info;

    /*
     * The volume of the simulator is a file of the machine: its size is the
     * size of the "flash". The build knows the size as well, which covers
     * the moment before the file is made.
     */
    if (stat(port_host_volume_path(), &info) == 0 && info.st_size > 0) {
        s_flash_size = (k_size_t)info.st_size;
    } else {
        s_flash_size = (k_size_t)MICROOS_VOLUME_SECTORS * PORT_HOST_VOLUME_SECTOR_SIZE;
    }

    return s_flash_size > 0 ? 0 : -1;
}

static int host_flash_check(void)
{
    return s_flash_size > 0 ? 0 : -1;
}

static const char *host_flash_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "host file, %u KB", (unsigned)(s_flash_size / 1024));
    return buf;
}

/* --- display --- */

static int host_display_init(void)
{
    return 0;
}

static int host_display_check(void)
{
    /* Without the file screen there is no renderer at all: degraded, not broken. */
    return host_screen_path() != NULL ? 0 : 1;
}

static const char *host_display_detail(void)
{
    return host_screen_path() != NULL ? "135x240, host renderer" : "no screen";
}

/* --- input --- */

static int host_input_init(void)
{
    return 0;
}

static bool host_input_script_ready(void)
{
    const char *path = host_input_script();

    return path != NULL && access(path, R_OK) == 0;
}

static int host_input_check(void)
{
    return host_input_script_ready() ? 0 : 1;
}

static const char *host_input_detail(void)
{
    return host_input_script_ready() ? "scripted" : "no script";
}

/* --- the clock --- */

static char s_rtc_line[24];

static int host_rtc_init(void)
{
    return 0;
}

static int host_rtc_check(void)
{
    host_clock_text(s_rtc_line, sizeof(s_rtc_line));
    return 0;
}

static const char *host_rtc_detail(void)
{
    return s_rtc_line;
}

/* --- the pieces that are simply not there --- */

static int host_noop_init(void)
{
    return 0;
}

static int host_noop_check(void)
{
    return 0;
}

static const char *host_battery_detail(void)
{
    return "fixed 4.0 V, 100%";
}

static const char *host_buzzer_detail(void)
{
    return "not connected";
}

static const char *host_led_detail(void)
{
    return "not connected";
}

static int host_wifi_check(void)
{
    /* The host has no radio: the station works only in principle. */
    return 1;
}

static const char *host_wifi_detail(void)
{
    return "no radio";
}

/* --- the registry --- */

static const struct kmod s_modules[] = {
    {
        .name = "psram",
        .desc = "the PSRAM pool of the host",
        .init = psram_module_init,
        .check = host_psram_check,
        .detail = host_psram_detail,
    },
    {
        .name = "flash",
        .desc = "the volume file of the simulator",
        .init = host_flash_init,
        .check = host_flash_check,
        .detail = host_flash_detail,
    },
    {
        .name = "rtc",
        .desc = "the clock of the host process",
        .init = host_rtc_init,
        .check = host_rtc_check,
        .detail = host_rtc_detail,
    },
    {
        .name = "display",
        .desc = "the text screen of the host",
        .init = host_display_init,
        .check = host_display_check,
        .detail = host_display_detail,
    },
    {
        .name = "input",
        .desc = "the scripted buttons of the host",
        .init = host_input_init,
        .check = host_input_check,
        .detail = host_input_detail,
    },
    {
        .name = "buzzer",
        .desc = "no buzzer on the host",
        .init = host_noop_init,
        .check = host_noop_check,
        .detail = host_buzzer_detail,
    },
    {
        .name = "led/ir",
        .desc = "no led on the host",
        .init = host_noop_init,
        .check = host_noop_check,
        .detail = host_led_detail,
    },
    {
        .name = "battery",
        .desc = "no battery on the host",
        .init = host_noop_init,
        .check = host_noop_check,
        .detail = host_battery_detail,
    },
    {
        .name = "wifi",
        .desc = "no radio on the host",
        .init = host_noop_init,
        .check = host_wifi_check,
        .detail = host_wifi_detail,
    },
    {
        /* No init and no check: kmod_doctor reports it as skipped. */
        .name = "imu",
        .desc = "no imu on the host",
    },
    {
        /* The board skips the microphone too, until the audio phase. */
        .name = "mic",
        .desc = "no microphone on the host",
    },
};

void host_modules_register(void)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(s_modules); i++) {
        (void)kmod_register(&s_modules[i]);
    }
}
