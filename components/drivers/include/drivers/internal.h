/*
 * Micro OS — internal interfaces of the driver component.
 */
#ifndef MICRO_OS_DRIVERS_INTERNAL_H
#define MICRO_OS_DRIVERS_INTERNAL_H

#include "kernel/types.h"

/* I2C bus shared by the IMU and the RTC. */
int drv_i2c_init(void);
int drv_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
int drv_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);
int drv_i2c_probe(uint8_t addr, uint32_t speed_hz, uint8_t *value);
int drv_i2c_set_speed(uint32_t speed_hz);
int drv_i2c_register_command(void);

/* Module registration. */
int drv_display_register(void);
int drv_input_register(void);
int drv_buzzer_register(void);
int drv_buzzer_beep(unsigned freq_hz, unsigned ms);
int drv_led_register(void);
int drv_imu_register(void);
int drv_rtc_register(void);
int drv_wifi_register(void);
int drv_memory_register(void);
int drv_battery_register(void);
int drv_mic_register(void);

#endif /* MICRO_OS_DRIVERS_INTERNAL_H */
