/*
 * Micro OS — shared I2C bus of the StickC Plus2.
 *
 * The IMU (MPU6886) and the RTC (BM8563) hang on the same bus:
 * SDA = G21, SCL = G22.
 */
#include <string.h>

#include "drivers/board_pins.h"
#include "drivers/drivers.h"

#include "driver/i2c_master.h"
#include "kernel/log.h"

static i2c_master_bus_handle_t s_bus;

int drv_i2c_init(void)
{
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (s_bus != NULL) {
        return 0;
    }

    if (i2c_new_master_bus(&config, &s_bus) != ESP_OK) {
        s_bus = NULL;
        return -1;
    }

    return 0;
}

int drv_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };

    if (s_bus == NULL) {
        return -1;
    }

    if (i2c_master_bus_add_device(s_bus, &config, &dev) != ESP_OK) {
        return -2;
    }

    if (i2c_master_transmit(dev, &reg, 1, true) != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return -3;
    }

    if (i2c_master_receive(dev, buf, len, true) != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return -4;
    }

    i2c_master_bus_rm_device(dev);
    return 0;
}

int drv_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    uint8_t tmp[16];
    esp_err_t rc;

    if (s_bus == NULL || len + 1 > sizeof(tmp)) {
        return -1;
    }

    if (i2c_master_bus_add_device(s_bus, &config, &dev) != ESP_OK) {
        return -2;
    }

    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);

    rc = i2c_master_transmit(dev, tmp, len + 1, true);
    i2c_master_bus_rm_device(dev);

    return rc == ESP_OK ? 0 : -3;
}
