/*
 * Micro OS — shared I2C bus of the StickC Plus2.
 *
 * The IMU (MPU6886) and the RTC (BM8563) hang on the same bus:
 * SDA = G21, SCL = G22.
 *
 * The bus belongs to two tasks at once (the shell with "i2c", the driver of
 * the radio with its own reads), so every transaction is done under one
 * lock: the register of a chip is a position in the chip, and two tasks
 * that move the position at the same time read each other's answers.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kernel/log.h"

/* The number of attempts of a single transaction. */
#define I2C_ATTEMPTS 3
#define I2C_RETRY_MS 10
#define I2C_TIMEOUT_MS 100

/*
 * The devices of the bus. The handle of a chip is made once and lives as
 * long as the bus does: adding and removing a device for every
 * transaction costs time, and a transaction that is abandoned in the
 * middle leaves the memory of the driver behind.
 */
#define I2C_DEVICE_MAX 4

struct i2c_entry {
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    uint32_t speed_hz;
};

static i2c_master_bus_handle_t s_bus;
static struct i2c_entry s_devices[I2C_DEVICE_MAX];
static SemaphoreHandle_t s_lock;
static uint32_t s_speed_hz = 100000;

/*
 * The handle of a chip, made the first time it is needed. The caller holds
 * the lock of the bus.
 */
static i2c_master_dev_handle_t i2c_device(uint8_t addr, uint32_t speed_hz, int *rc)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = speed_hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    size_t i;
    size_t empty = I2C_DEVICE_MAX;

    for (i = 0; i < I2C_DEVICE_MAX; i++) {
        if (s_devices[i].dev != NULL && s_devices[i].addr == addr &&
            s_devices[i].speed_hz == speed_hz) {
            *rc = 0;
            return s_devices[i].dev;
        }

        if (s_devices[i].dev == NULL && empty == I2C_DEVICE_MAX) {
            empty = i;
        }
    }

    if (empty == I2C_DEVICE_MAX) {
        *rc = -2; /* the table of the devices is full */
        return NULL;
    }

    if (i2c_master_bus_add_device(s_bus, &config, &dev) != ESP_OK) {
        *rc = -2;
        return NULL;
    }

    s_devices[empty].dev = dev;
    s_devices[empty].addr = addr;
    s_devices[empty].speed_hz = speed_hz;
    *rc = 0;

    return dev;
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

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return -1;
        }
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
    i2c_master_dev_handle_t dev;
    int rc = -3;
    int attempt;

    if (s_bus == NULL || buf == NULL) {
        return -1;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    dev = i2c_device(addr, s_speed_hz, &rc);
    if (dev == NULL) {
        xSemaphoreGive(s_lock);
        return rc;
    }

    for (attempt = 0; attempt < I2C_ATTEMPTS; attempt++) {
        /*
         * The register and its answer are one transaction: no other task
         * can put its own register in the middle of it, and the chip is
         * never left with a register pointer that it was not asked for.
         */
        if (i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS) == ESP_OK) {
            rc = 0;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_MS));
    }

    xSemaphoreGive(s_lock);

    return rc;
}

int drv_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];
    i2c_master_dev_handle_t dev;
    int rc = -3;
    int attempt;

    if (s_bus == NULL || buf == NULL || len + 1 > sizeof(tmp)) {
        return -1;
    }

    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    dev = i2c_device(addr, s_speed_hz, &rc);
    if (dev == NULL) {
        xSemaphoreGive(s_lock);
        return rc;
    }

    for (attempt = 0; attempt < I2C_ATTEMPTS; attempt++) {
        if (i2c_master_transmit(dev, tmp, len + 1, I2C_TIMEOUT_MS) == ESP_OK) {
            rc = 0;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_MS));
    }

    xSemaphoreGive(s_lock);

    return rc;
}

/*
 * Probe an address: write a zero byte and look at the acknowledgement.
 * Returns 0 when a device answers, and puts the value of its register 0
 * into value when it is asked for.
 */
int drv_i2c_probe(uint8_t addr, uint32_t speed_hz, uint8_t *value)
{
    uint8_t reg = 0x00;
    i2c_master_dev_handle_t dev;
    int rc;

    if (s_bus == NULL) {
        return -1;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    dev = i2c_device(addr, speed_hz, &rc);
    if (dev == NULL) {
        xSemaphoreGive(s_lock);
        return rc;
    }

    if (value != NULL) {
        uint8_t tmp = 0;

        rc = (i2c_master_transmit_receive(dev, &reg, 1, &tmp, 1, I2C_TIMEOUT_MS) == ESP_OK) ? 0
                                                                                            : -1;
        *value = tmp;
    } else {
        rc = (i2c_master_transmit(dev, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK) ? 0 : -1;
    }

    xSemaphoreGive(s_lock);

    return rc;
}
