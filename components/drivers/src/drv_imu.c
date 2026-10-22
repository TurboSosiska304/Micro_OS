/*
 * Micro OS — IMU driver (MPU6886) of the StickC Plus2.
 *
 * The sensor sits on the I2C bus at address 0x68. The check reads the
 * identification register and one acceleration sample, so that a broken
 * solder joint or a wrong address is detected.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <stdio.h>

#include "kernel/kmod.h"

#define MPU_REG_WHO_AM_I 0x75
#define MPU_REG_PWR_MGMT1 0x6B
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_ACCEL_CONFIG 0x1C /* the range of the acceleration */
#define MPU_REG_GYRO_CONFIG 0x1B  /* the range of the rotation */
#define MPU_REG_CONFIG 0x1A       /* the filter of the rotation (DLPF) */

static bool s_present;
static int16_t s_accel[3];

static int drv_imu_init(void)
{
    uint8_t who = 0;
    uint8_t config = 0;

    if (drv_i2c_init() != 0) {
        return -1;
    }

    if (drv_i2c_read(I2C_MPU6886_ADDR, MPU_REG_WHO_AM_I, &who, 1) != 0) {
        return -2;
    }

    /* MPU6886 answers with 0x19 (some revisions with 0x1B). */
    if (who != 0x19 && who != 0x1B && who != 0x70 && who != 0x71) {
        return -3;
    }

    /*
     * Wake up, +-2 g, +-250 deg/s, the filter of the rotation at 44 Hz.
     * Every value is written to the register that holds it, and every
     * write is checked: a chip that stopped answering must not be
     * reported as working.
     */
    config = 0x01; /* the clock of the chip from the crystal */
    if (drv_i2c_write(I2C_MPU6886_ADDR, MPU_REG_PWR_MGMT1, &config, 1) != 0) {
        return -4;
    }

    config = 0x00; /* +-2 g: the whole range of a board on a table */
    if (drv_i2c_write(I2C_MPU6886_ADDR, MPU_REG_ACCEL_CONFIG, &config, 1) != 0) {
        return -5;
    }

    config = 0x00; /* +-250 deg/s */
    if (drv_i2c_write(I2C_MPU6886_ADDR, MPU_REG_GYRO_CONFIG, &config, 1) != 0) {
        return -6;
    }

    config = 0x03; /* the filter at 44 Hz: the noise goes, the motion stays */
    if (drv_i2c_write(I2C_MPU6886_ADDR, MPU_REG_CONFIG, &config, 1) != 0) {
        return -7;
    }

    s_present = true;
    return 0;
}

static int drv_imu_check(void)
{
    uint8_t raw[6];
    int i;

    if (!s_present) {
        return -1;
    }

    if (drv_i2c_read(I2C_MPU6886_ADDR, MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw)) != 0) {
        return -2;
    }

    for (i = 0; i < 3; i++) {
        s_accel[i] = (int16_t)((raw[i * 2] << 8) | raw[i * 2 + 1]);
    }

    return 0;
}

static const char *drv_imu_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "acc %d,%d,%d (raw)", s_accel[0], s_accel[1], s_accel[2]);
    return buf;
}

static const struct kmod s_imu_module = {
    .name = "imu",
    .desc = "MPU6886 6-axis IMU on I2C",
    .init = drv_imu_init,
    .check = drv_imu_check,
    .detail = drv_imu_detail,
};

int drv_imu_register(void)
{
    return kmod_register(&s_imu_module);
}
