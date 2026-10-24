/*
 * Micro OS — how much charge a lithium cell has left, in percent.
 *
 * The battery is not a fuel gauge: nobody counts the milliamp-hours in
 * it, the curve of a single cell is looked up instead — full at
 * BATTERY_FULL_MV, empty at BATTERY_EMPTY_MV, and the flat plateau of
 * the chemistry in between. It is pure arithmetic with no hardware in
 * it, which is why the host tests compile the file themselves and run
 * voltages through it.
 *
 * The numbers are for a cell that rests: under load a cell sags, and
 * while the board is on the charger the divider sees the charging
 * voltage, which is above the top of the curve (so it reads 100%).
 */
#ifndef MICRO_OS_DRIVERS_BATTERY_SOC_H
#define MICRO_OS_DRIVERS_BATTERY_SOC_H

/* The ends of the curve: a resting cell above full is full, below empty is empty. */
#define BATTERY_FULL_MV 4200
#define BATTERY_EMPTY_MV 3000

/*
 * The charge the curve tells for a voltage: 100% at and above
 * BATTERY_FULL_MV, 0% at and below BATTERY_EMPTY_MV, and a straight
 * line between the points of the curve. Everything outside the curve is
 * clamped into it, so the answer is always 0..100.
 */
int battery_soc_percent(int mv);

#endif /* MICRO_OS_DRIVERS_BATTERY_SOC_H */
