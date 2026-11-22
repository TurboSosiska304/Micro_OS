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
    "src/drv_ir.c"
    "src/drv_memory.c"
    "src/drv_mic.c"
    "src/drv_power.c"
    "src/drv_rtc.c"
    "src/drv_sound.c"
    "src/fb_draw.c"
    "src/input_debounce.c"
    "src/ir_nec.c"
    "src/battery_soc.c"
    "src/rtc_time.c"
    "src/sound_detect.c"
)

# The driver of the station is the glue of the portable core (MICRO_OS_WIFI,
# ADR 0010): a build without the station registers no module, and the source
# is not compiled.
if(CONFIG_MICRO_OS_WIFI)
    list(APPEND MICRO_OS_DRIVERS_SRCS "src/drv_wifi.c")
endif()

idf_component_register(
    SRCS ${MICRO_OS_DRIVERS_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel font kshell devfs freertos wifi spi_flash esp_psram
             esp_driver_gpio esp_driver_i2c driver esp_driver_spi esp_driver_ledc
             esp_adc esp_driver_i2s esp_driver_rmt esp_timer esp_wifi esp_netif
             esp_event nvs_flash graphics settings
             esp_hw_support
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
