# M5Stack StickC Plus2 hardware

> Russian version: [stickc-plus2_ru.md](stickc-plus2_ru.md)

Source: the official M5Stack documentation (SKU **K016-P2**).
Check everything below before writing a driver: the board has no PMIC, an unusual
power model and a shared IR/LED pin.

## Main features

| Parameter | Value |
|---|---|
| SoC | ESP32-PICO-V3-02 (Xtensa LX6, 240 MHz, 2 cores) |
| Flash | 8 MB SPI |
| PSRAM | 2 MB Quad |
| Display | 1.14" TFT, 135x240, ST7789v2 |
| IMU | MPU6886 (accelerometer + gyroscope) |
| RTC | BM8563 |
| Microphone | SPM1423 (I2S) |
| Battery | 200 mAh, 3.7 V |
| USB | Type-C, CH9102 USB-UART chip |
| Size | 48 x 24 x 13.5 mm, ~17 g |
| Power supply | 5 V / 500 mA via USB |

## Pinout

### Display (SPI, ST7789v2)

| Signal | GPIO |
|---|---|
| TFT_MOSI | G15 |
| TFT_CLK | G13 |
| TFT_DC | G14 |
| TFT_RST | G12 |
| TFT_CS | G5 |
| TFT_BL | G27 |

### Buttons, buzzer, IR, LED

| Signal | GPIO | Note |
|---|---|---|
| BUTTON A | G37 | |
| BUTTON B | G39 | |
| BUTTON C | G35 | power / reset / wake |
| BUZZER | G2 | passive, needs PWM |
| IR transmitter | G19 | **shares the pin with the red LED** |
| Red LED | G19 | not controllable separately from IR |
| Green LED | - | sleep indicator, not programmable |
| HOLD (power) | G4 | **must be driven high after a wake-up** |
| Battery ADC | G38 | battery voltage divider |

### I2C bus

| Signal | GPIO |
|---|---|
| SCL | G22 |
| SDA | G21 |

Devices on that bus:

| Device | Address |
|---|---|
| MPU6886 (IMU) | 0x68 |
| BM8563 (RTC) | 0x51 |

### Microphone (I2S)

| Signal | GPIO |
|---|---|
| MIC CLK | G0 |
| MIC DATA | G34 |

### Grove / external pins

| Pin | GPIO | Note |
|---|---|---|
| PORT.CUSTOM (Grove) | G32, G33 | plus 5 V and GND; G32/G33 are I2C or UART |
| G0 | G0 | free (used by the microphone!) |
| G25 / G36 | G25, G36 | **G25 and G36 share one input** — the other must be float |
| G26 | G26 | free |

## Power and sleep (important!)

* The **AXP192 PMIC is absent** — charging and power are handled by the ESP32.
* Power on: hold **BUTTON C** for more than 2 seconds.
* After a wake-up from the RTC or the button, the program must set
  **HOLD (G4) = 1**, otherwise the board powers off again.
* Power off: hold BUTTON C (or call `esp_restart()` / `esp_deep_sleep`).
* Battery charging: with USB power the battery charges; the charge current and
  the state are read through the ADC divider on G38.

## Known pitfalls

* **C-to-C cable**: the port may not be detected. Fix: power the board off (long
  press until the green LED lights up), then reconnect the cable.
* **PSRAM is mandatory** in `sdkconfig`: without `CONFIG_SPIRAM=y` there is no
  2 MB of RAM and the `esp_psram` frame buffers will not work.
* **G25 and G36**: if one of them is used as an ADC input, the other must be
  switched to float (`gpio_pulldown_dis(GPIO_NUM_25)` / `gpio_pullup_dis`).
* **G19**: driving the IR diode turns the red LED off and vice versa.
* **Display**: the Plus2 uses different DC/RST pins (G14/G12) than the StickC and
  StickC Plus (G23/G18). Code for the StickC Plus will not run on the Plus2.
* **USB-UART driver**: the Plus2 uses the CH9102 (it needs its own VCP driver),
  not CH552/FTDI as on the StickC/Plus.

## Clocks and timers

* Main clock: 240 MHz.
* Exact 32.768 kHz RTC oscillator — it is also the deep-sleep alarm source.
* ESP32 timers: two blocks of four timers each, 64 bit — used for the system tick.

## Sources

* Documentation: https://docs.m5stack.com/en/core/M5StickC%20PLUS2
* Schematic and pin map: the "PinMap" section of the documentation, PDF on
  docs.m5stack.com
* StickC / StickC Plus / StickC Plus2 comparison: the "Version Change" table in
  the documentation
