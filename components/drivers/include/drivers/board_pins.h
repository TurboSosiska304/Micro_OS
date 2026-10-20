/*
 * Micro OS — pin map of the M5Stack StickC Plus2.
 *
 * Source: the official M5Stack documentation (SKU K016-P2).
 */
#ifndef MICRO_OS_DRIVERS_BOARD_PINS_H
#define MICRO_OS_DRIVERS_BOARD_PINS_H

/* Display (ST7789v2, SPI) */
#define PIN_LCD_MOSI 15
#define PIN_LCD_CLK 13
#define PIN_LCD_CS 5
#define PIN_LCD_DC 14
#define PIN_LCD_RST 12
#define PIN_LCD_BL 27

#define LCD_WIDTH 135
#define LCD_HEIGHT 240

/* Buttons */
#define PIN_BUTTON_A 37
#define PIN_BUTTON_B 39
#define PIN_BUTTON_C 35

/* Buzzer (passive, PWM) */
#define PIN_BUZZER 2

/* Red LED and IR emitter share this pin */
#define PIN_LED 19

/* I2C bus */
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define I2C_PORT 0
#define I2C_MPU6886_ADDR 0x68
#define I2C_BM8563_ADDR 0x51

/* Microphone (I2S) */
#define PIN_MIC_CLK 0
#define PIN_MIC_DATA 34

/* Power hold: must be driven high to keep the board powered */
#define PIN_HOLD 4

/* Battery voltage divider */
#define PIN_BATTERY_ADC 38

#endif /* MICRO_OS_DRIVERS_BOARD_PINS_H */
