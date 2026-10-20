/*
 * Micro OS — shared I2C bus of the StickC Plus2.
 *
 * The IMU (MPU6886) and the RTC (BM8563) hang on the same bus:
 * SDA = G21, SCL = G22.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/log.h"

/* The number of attempts of a single transaction. */
#define I2C_ATTEMPTS 3
#define I2C_RETRY_MS 10
#define I2C_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_bus;
static uint32_t s_speed_hz = 100000;

static int i2c_add_device(uint8_t addr, uint32_t speed_hz, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = speed_hz,
    };

    return i2c_master_bus_add_device(s_bus, &config, dev) == ESP_OK ? 0 : -1;
}

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

    /*
     * The sensors need a moment after the power up. The first transaction is
     * often NACKed, the retries below take care of that.
     */
    vTaskDelay(pdMS_TO_TICKS(100));

    return 0;
}

int drv_i2c_set_speed(uint32_t speed_hz)
{
    if (speed_hz == 0) {
        return -1;
    }

    s_speed_hz = speed_hz;
    return 0;
}

int drv_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    int attempt;

    if (s_bus == NULL) {
        return -1;
    }

    for (attempt = 0; attempt < I2C_ATTEMPTS; attempt++) {
        i2c_master_dev_handle_t dev;

        if (i2c_add_device(addr, s_speed_hz, &dev) != 0) {
            return -2;
        }

        if (i2c_master_transmit(dev, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(dev, buf, len, I2C_TIMEOUT_MS) == ESP_OK) {
            i2c_master_bus_rm_device(dev);
            return 0;
        }

        i2c_master_bus_rm_device(dev);
        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_MS));
    }

    return -3;
}

int drv_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];
    int attempt;

    if (s_bus == NULL || len + 1 > sizeof(tmp)) {
        return -1;
    }

    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);

    for (attempt = 0; attempt < I2C_ATTEMPTS; attempt++) {
        i2c_master_dev_handle_t dev;

        if (i2c_add_device(addr, s_speed_hz, &dev) != 0) {
            return -2;
        }

        if (i2c_master_transmit(dev, tmp, len + 1, I2C_TIMEOUT_MS) == ESP_OK) {
            i2c_master_bus_rm_device(dev);
            return 0;
        }

        i2c_master_bus_rm_device(dev);
        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_MS));
    }

    return -3;
}

/*
 * Probe an address: write a zero byte and look at the acknowledgement.
 * Returns 0 when a device answers.
 */
int drv_i2c_probe(uint8_t addr, uint32_t speed_hz, uint8_t *value)
{
    uint8_t reg = 0x00;
    i2c_master_dev_handle_t dev;
    int rc = -1;

    if (s_bus == NULL) {
        return -1;
    }

    if (i2c_add_device(addr, speed_hz, &dev) != 0) {
        return -1;
    }

    if (i2c_master_transmit(dev, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK) {
        if (value != NULL) {
            uint8_t tmp = 0;

            (void)i2c_master_receive(dev, &tmp, 1, I2C_TIMEOUT_MS);
            *value = tmp;
        }
        rc = 0;
    }

    i2c_master_bus_rm_device(dev);
    return rc;
}
