/*
 * Micro OS — battery voltage of the StickC Plus2.
 *
 * The battery is connected to G38 through a divider. The reading is only
 * meaningful when the board runs on battery: while the USB cable is
 * connected the divider sees the charging voltage.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <stdio.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "kernel/kmod.h"

/* Ratio of the divider on the board. */
#define BATTERY_DIVIDER 2.0f

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static bool s_ready;
static int s_mv;

static int drv_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    if (adc_oneshot_new_unit(&unit_cfg, &s_unit) != ESP_OK) {
        return -1;
    }

    if (adc_oneshot_config_channel(s_unit, ADC_CHANNEL_2, &chan_cfg) != ESP_OK) {
        return -2;
    }

    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        /* Without the calibration the raw value is still usable. */
        s_cali = NULL;
    }

    s_ready = true;
    return 0;
}

static int drv_battery_check(void)
{
    int raw = 0;
    int mv = 0;

    if (!s_ready) {
        return -1;
    }

    if (adc_oneshot_read(s_unit, ADC_CHANNEL_2, &raw) != ESP_OK) {
        return -2;
    }

    if (s_cali != NULL) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return -3;
        }
    } else {
        mv = raw * 3100 / 4095;
    }

    s_mv = mv;
    return 0;
}

static const char *drv_battery_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%d mV (raw %d mV)", (int)(s_mv * BATTERY_DIVIDER), s_mv);
    return buf;
}

static const struct kmod s_battery_module = {
    .name = "battery",
    .desc = "battery voltage on G38",
    .init = drv_battery_init,
    .check = drv_battery_check,
    .detail = drv_battery_detail,
};

int drv_battery_register(void)
{
    return kmod_register(&s_battery_module);
}
