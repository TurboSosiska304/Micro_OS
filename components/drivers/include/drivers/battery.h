/*
 * Micro OS — the battery of the board.
 *
 * The battery is connected to G38 through a divider, and the reading is
 * only the truth when the board runs on battery: while the USB cable is
 * connected the divider sees the charging voltage, which is above the
 * top of the curve (see drivers/battery_soc.h).
 */
#ifndef MICRO_OS_DRIVERS_BATTERY_H
#define MICRO_OS_DRIVERS_BATTERY_H

/*
 * The voltage of the battery right now, in millivolts. Every call takes
 * a fresh reading of the ADC; 0 means the driver is not ready.
 *
 * The charge it means is the same number through the curve of one cell:
 * battery_soc_percent(drv_battery_voltage_mv()) — the curve is public
 * in drivers/battery_soc.h, with no ADC in it.
 */
int drv_battery_voltage_mv(void);

#endif /* MICRO_OS_DRIVERS_BATTERY_H */
