/*
 * Micro OS — I2C bus diagnostic.
 *
 * Scans the bus and reports the addresses that answer. The scan tries
 * several clock speeds: the internal pull-ups of the ESP32 are weak, so a
 * slow bus can be the difference between a working and a dead sensor.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <stdio.h>

#include "kernel/kmod.h"
#include "kshell/kshell.h"

static const uint32_t s_speeds[] = {400000, 100000, 50000, 10000};

static int drv_i2c_scan(int argc, char **argv)
{
    size_t i;
    int total = 0;

    (void)argc;
    (void)argv;

    if (drv_i2c_init() != 0) {
        kshell_printf("i2c: the bus is not available\n");
        return KSHELL_ERROR;
    }

    kshell_printf("i2c: SDA=G%d, SCL=G%d\n", PIN_I2C_SDA, PIN_I2C_SCL);

    for (i = 0; i < sizeof(s_speeds) / sizeof(s_speeds[0]); i++) {
        uint32_t speed = s_speeds[i];
        int addr;
        int found = 0;

        for (addr = 0x08; addr <= 0x77; addr++) {
            uint8_t value = 0;

            if (drv_i2c_probe((uint8_t)addr, speed, &value) == 0) {
                kshell_printf("  %6u Hz: 0x%02x\n", (unsigned)speed, addr);
                found++;
            }
        }

        if (found == 0) {
            kshell_printf("  %6u Hz: nothing\n", (unsigned)speed);
        }

        total += found;
    }

    if (total == 0) {
        kshell_printf("i2c: no device answered\n");
        return KSHELL_ERROR;
    }

    kshell_printf("i2c: %d answer(s) in total\n", total);
    return KSHELL_OK;
}

int drv_i2c_register_command(void)
{
    return kshell_register("i2c", "scan the I2C bus", drv_i2c_scan);
}
