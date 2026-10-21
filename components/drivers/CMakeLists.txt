# Micro OS — device drivers of the M5Stack StickC Plus2.
#
# The component is ESP32 only: the drivers talk to the hardware directly.

set(MICRO_OS_DRIVERS_SRCS
    "src/drivers.c"
    "src/drv_battery.c"
    "src/drv_buzzer.c"
    "src/drv_display.c"
    "src/drv_i2c.c"
    "src/drv_i2c_scan.c"
    "src/drv_imu.c"
    "src/drv_input.c"
    "src/drv_memory.c"
    "src/drv_mic.c"
    "src/drv_rtc.c"
    "src/drv_wifi.c"
)

idf_component_register(
    SRCS ${MICRO_OS_DRIVERS_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel font kshell spi_flash esp_psram esp_driver_gpio esp_driver_i2c driver esp_driver_spi esp_driver_ledc esp_adc esp_timer
             esp_wifi esp_netif esp_event nvs_flash
)
