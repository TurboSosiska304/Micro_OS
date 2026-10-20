/*
 * Micro OS — registration of all the drivers.
 *
 * The order matters a little: the I2C bus is used by the IMU and the RTC,
 * the display and the WiFi radio are independent of each other.
 */
#include "drivers/drivers.h"
#include "drivers/internal.h"

#include "kernel/kmod.h"
#include "kernel/log.h"

int drivers_init(void)
{
    int failed = 0;

    failed += drv_memory_register() != 0;
    failed += drv_imu_register() != 0;
    failed += drv_rtc_register() != 0;
    failed += drv_display_register() != 0;
    failed += drv_input_register() != 0;
    failed += drv_buzzer_register() != 0;
    failed += drv_led_register() != 0;
    failed += drv_battery_register() != 0;
    failed += drv_wifi_register() != 0;
    failed += drv_mic_register() != 0;
    drv_i2c_register_command();

    if (failed != 0) {
        kerror("drivers: %d module(s) could not be registered", failed);
        return -1;
    }

    return 0;
}
