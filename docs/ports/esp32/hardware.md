# M5Stack StickC Plus2 hardware

> Russian version: [../../ru/ports/esp32/hardware_ru.md](../../ru/ports/esp32/hardware_ru.md)

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
| IR transmitter | G19 | **shares the pin with the red LED**; the same pad, one diode or the other |
| Red LED | G19 | not controllable separately from IR |
| IR receiver (external) | G26 | **not on the board**: a demodulating element a person wires there |
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

The INT of the BM8563 is not a GPIO of the ESP32 in the schematics: it drives
the latch of the power supply through D22, so an alarm can switch on a board
that is off. It is not on the EXT1 wake line either, so the chip cannot wake
on it by itself — the firmware arms its own timer for the moment of the alarm
instead (see "Power and sleep").

### Microphone (I2S)

| Signal | GPIO |
|---|---|
| MIC CLK | G0 |
| MIC DATA | G34 |

The SPM1423 is a MEMS element with a PDM output: there is no register to
read and no identification to ask for, the element simply answers with a
stream of bits. The driver (`drv_mic.c`, `CONFIG_MICRO_OS_MIC`) therefore
runs the I2S peripheral in **PDM RX** mode, mono, 16 kHz, with the clock
generated on G0 and the data sampled on G34, and the hardware doctor proves
the microphone with a short window of signal: it reports the RMS and the
peak of the audio around the converter's own offset.

Limits worth knowing before writing an application around it:

* G0 is a strapping pin of the ESP32 (it selects the download mode at
  reset). The microphone only takes it as an output after the boot, but a
  Grove device that pulls G0 low while the board resets keeps the board in
  the loader.
* G34 is an input-only pin without pull-up or pull-down; the element drives
  it, nothing else should.
* The channel runs from the boot and owns I2S0. An application that needs
  the peripheral for something else has to turn the driver off in the
  configuration (`CONFIG_MICRO_OS_MIC=n`) and take the channel itself.
* The doctor cannot tell a silent room from a dead element by listening
  alone: a window under both limits (RMS 2 and peak 8; a quiet room reads
  about 20 and 60 on the module of this tree) is reported as `degraded` with
  the numbers, not as `ok`.
* A quiet room of this board reads an RMS of about 20 in a window of 50 ms,
  which is the number the sound detector is built on: its threshold is 200 by
  default, ten times the room, and an event needs two windows in a row above
  it (see "The sound detector").

### The IR pin (G19) and the red LED

One pad, two diodes, and no way to light one without the other: the driver
makes the rule of the wiring explicit instead of letting two modules write the
same pad.

* The **emitter owns G19 while a frame goes out.** The light is made dark
  first, the pad is handed to the RMT peripheral, the carrier of 38 kHz
  carries the pulses, and the pad goes back to the LEDC afterwards, dark. A
  `led on` that arrives during a frame answers `the IR emitter has G19` and
  changes nothing.
* The **light owns the pad the rest of the time.** The LEDC keeps its duty,
  the shows (`led heart`, `led dance`) run as before, and a frame that
  arrives takes the pad away for its 68 ms.

```
ir                         # who owns G19, and what the emitter would send
ir frame 04 08             # the frame of a NEC command: 34 pulses, 68 ms, not sent
ir send 04 08              # and this one leaves the emitter
ir rx 5000                 # waits for a demodulating receiver on G26
```

The protocol is NEC (a 9 ms header, four bytes and their complements, the
address and the command of the cheap remotes of the world). The receiver is
**not on the board**: the Plus2 carries the emitter only. `ir rx` listens on
G26, the free pin of the Grove connector, for a demodulating element (a
38 kHz receiver such as a TSOP) that a person wires there; with nothing
attached the line stays idle and the command says so. The transmission has no
such limit and is checked in three ways: `ir frame` proves the timing and the
arithmetic in the doctor and in `tools/device-test.sh`; a person can see the
diode of `ir send` through the camera of a phone (an IR emitter looks like a
pale violet dot on a screen that cannot see it); and a probe on G19 shows the
bursts of the carrier — a 9 ms burst, a 4.5 ms gap and the 32 bits of the
frame, each a 560 us burst with a gap of 560 or 1690 us.

What no test of this tree has run is the reception: the decode of a real
frame from a real remote. The path is written (RMT on G26, inverted input, the
tolerance of ir_nec.c) and the reader is covered by the host tests, but the
element is not on the board and the tree cannot pretend otherwise.

### The sound detector

The detector is a rule on top of the microphone and not a second driver of the
element: `drv_mic` reads windows of 16 kHz audio and answers with RMS and
peak, and the detector says "somebody is making a sound" when two windows in
a row are at or above a threshold of RMS. The quiet room of this board reads
about 20, the threshold is 200 by default, and the band between half of it
and the threshold is where a fading sound lives — that is what keeps a word
from counting as ten events.

```
sound                      # the threshold, the state and the events
sound listen 3000          # three seconds, the loudest window and the events
sound threshold 400        # a room that is louder than the default
sound watch on             # a task watches the room; every event is a line of the log
sound watch off
```

The hardware doctor listens to a settled microphone and reports `quiet: rms N,
peak M, threshold T`; a room that is loud at the boot is `degraded` with the
numbers, not a fault, and a detector that starts on a quiet window is the
fault the check is for. The detector does not survive a deep sleep and is not
a wake source: the microphone is off while the chip sleeps, and the wearable
of P3 wakes on its timer, its clock and its button.

### Grove / external pins

| Pin | GPIO | Note |
|---|---|---|
| PORT.CUSTOM (Grove) | G32, G33 | plus 5 V and GND; G32/G33 are I2C or UART |
| G0 | G0 | free (used by the microphone!) |
| G25 / G36 | G25, G36 | **G25 and G36 share one input** — the other must be float |
| G26 | G26 | free |

## Power and sleep (important!)

* The **AXP192 PMIC is absent** — charging and power are handled by the ESP32.
* Power on: hold **BUTTON C** for more than 2 seconds, or let the alarm of the
  RTC switch the board on.
* After a wake-up from the RTC or the button, the program must set
  **HOLD (G4) = 1**, otherwise the board powers off again. The firmware does
  it in the first statement of `app_main`, before the console or the kernel.
* Power off: hold BUTTON C. The shell's `power off` is the clean end of
  ADR 0050: it syncs the volume, flushes the flash, and then holds HOLD and
  enters the deep sleep whose only wake source is BUTTON C. The board is off
  until a person presses the button, and it boots again from the clean volume;
  the latch is kept, because a sleep with no way back is a dead board.
* Battery charging: with USB power the battery charges; the charge current and
  the state are read through the ADC divider on G38.

### Deep sleep and the wake line

The chip sleeps in the deep sleep of the ESP32: everything but the RTC domain
is off, `app_main` runs again from the start after the wake, and the firmware
drives HOLD high in its first statement. Two things bring the board back:

| Source | How |
|---|---|
| the timer of the chip | `power sleep <seconds>` in the shell; the kernel tick stops with the chip |
| BUTTON C (G35) | the button closes the **WAKEn** net to ground and the diode D1 carries the low to G35 |
| the alarm of the RTC | the firmware sets the timer of the chip for the moment of the alarm, because the INT of the clock is not a wake line |

The INT of the BM8563 drives the latch of the power supply and not the wake
line of the chip, so the alarm cannot wake a sleeping chip by itself. What the
firmware does instead is what the wiring leaves: `power sleep` asks the clock
for the seconds to the next match and arms the timer of the chip for that
moment, and the flag of the clock (`AF`) tells the next boot that the timer
and the alarm were the same event. The boot says which one it was —
`rtc: the alarm woke the board` in the RAM boot log, and `power` on the
console says the same — and the doctor shows the line of the clock with the
alarm that is armed. From the **off** state the alarm switches the board on
through the same latch, which is the way the product documentation describes
the RTC wake; the firmware cannot check that part with the cable plugged in,
because USB feeds the 3.3 V rail on its own.

```
power sleep 300            # five minutes, or the alarm when it comes sooner
power sleep alarm          # until the next match of the armed alarm
power                      # hold, the reason of the boot, and the alarm
```

The deep sleep arms EXT1 on G35 with the level "all low" — the only group
level the ESP32 has — and the timer beside it, and it keeps HOLD (G4) by the
pad hold of the RTC domain: without the hold the pin floats, the latch opens
and a board on a battery switches itself off instead of sleeping. The boot
reports the reason in one line of the log: `power: woke from timer`, `ext1` or
`power-on`, and the doctor shows it as `hold on, wake timer`.

G35 is input-only and has **no internal pull-up**: the 10 kOhm resistor of the
WAKEn net is what holds it high. A board whose line reads low never sleeps —
the wake fires in the same instant — so an external pull-down on G35 (or a
jammed button) is the first thing to look at.

The panel keeps the picture it had when the chip went down, and the green LED
of the board is the sleep indicator. The current of the deep sleep is not
measured by the firmware: the radio and the panel decide it, and only the
hardware can tell.

### The alarm of the RTC (BM8563)

The clock has one alarm: a match of a minute, an hour and a day of the month.
The weekday register is not used — the driver computes the weekday from the
date, and after a dead battery the register of the chip means nothing. A field
that is not given is not compared (the AE bit of its register is set), so an
alarm on `12:34` with no day fires every day until it is cleared.

```
rtc alarm                  # what is armed, and whether it fired
rtc alarm 07:30            # every day at 07:30
rtc alarm +600             # the next whole minute at or after now + 600 s
rtc alarm day 5 07:30      # on the 5th of every month
rtc alarm clear            # disable it and release the INT line
```

The chip compares minutes: `+30` fires at the start of the next minute, not in
thirty seconds. **The alarm survives a restart** — and a formatted volume, a
factory reset and a dead backup battery of the clock — because it lives in the
BM8563 and not in the store: the boot reads the flag of an alarm that fired
(the line stays low until somebody answers it), clears the flag, and leaves
the comparison of the chip enabled. A board that was told to wake at 07:30
keeps that promise across the nights; `rtc alarm clear` is the way to take it
back, and `power off` clears it too, because the INT line would otherwise hold
the latch of the power supply closed.

While the board runs, the INT line of a fired alarm sits low. The line holds
the latch of the power supply closed, so `power off` clears the alarm first, or
the board would stay on with HOLD dropped.

**The bits of the control register are not the PCF8563 ones.** The BM8563
orders register 01H as `TIE, AIE, TF, AF` from bit 0 up, where the PCF8563 has
`AIE, AF, TIE, TF`. A driver copied from the PCF8563 writes 0x01 to enable the
alarm — which arms the timer of the chip — and then polls bit 1 for the flag,
which never moves. That was the first version of `drv_rtc.c`; the alarm fired
on time and nobody noticed.

### Factory reset

`factory reset -y` erases the volume of MicroFS (every file of the user and
the boot log) and the whole NVS partition (the files of `/etc`, the network of
the last time, the calibration of the radio), writes the files of `/etc` that
the model has, and restarts the board. The firmware and the bootloader live in
partitions of their own and are not touched, so the board comes back with the
same firmware and none of the last life. Without `-y` the command explains
what would be lost and changes nothing; there is no undo.

The files of `/etc` of the model are `/etc/setting`
(`hello`, `hostname=stickc-plus2`, `console=tft`) and the directory
`/etc/init.d`; the boot writes them whenever one is missing, so a reset, a
`mkfs -y` and a first boot all end in the same tree, and the NVS keeps its
copy like of any other file. A file of the person is never replaced: the
default is written only when the volume does not have it.

The alarm of the clock is not a file and the reset does not take it away: a
board that is reset on a battery still wakes at its alarm, and `rtc alarm`
after the next boot shows the same minute. That is deliberate — an off board
whose only wake source was erased by a reset is a board nobody can turn on
without the cable.

## The memory of a program

The ESP32 has no MMU and two kinds of memory that a program needs both of
(ADR 0018):

| Memory | Range | What it can do | Window of a program |
|---|---|---|---|
| instruction RAM | `0x40080000`..`0x400A0000` (128 KiB) | fetch instructions, 32-bit data only | `0x4009E000`..`0x400A0000` (8 KiB) |
| DRAM (static) | `0x3FFB0000`.. up to the fixed size | byte-addressed data, not executable | `0x3FFCC000`..`0x3FFCE000` (8 KiB) |
| PSRAM | `0x3F800000`.. | byte-addressed data, not executable | none: a program's data is linked |

The instruction RAM is not byte-addressed. Without
`CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY` a `l8ui`/`s8i` of an address in
`0x400xxxxx` raises `LoadStoreError`; with the option, a handler emulates it at
up to 167 cycles per access. A C program reads its strings byte by byte, so the
loader puts the code in IRAM and the data in DRAM rather than enabling the
emulation for every character of a `printf`.

The DRAM window is the top of the static segment, and the build sets
`CONFIG_ESP32_USE_FIXED_STATIC_RAM_SIZE=0x1E000` so that the heap begins at
`0x3FFCE000` whatever the static sections do. The free IRAM at the time of the
loader was 15 054 bytes (`idf.py size`); 8 KiB of it is the window of the text
and the linker refuses a kernel that reaches it
(`components/port_esp32/exec_windows.ld`). The text window is also reserved
from the IRAM heap at startup, because an allocation with `MALLOC_CAP_EXEC`
prefers IRAM; the other pools of instruction RAM stay in the heap (96 076 bytes
were free after the boot). The program itself has an 8 KiB stack: the stack of
the kernel task it runs in (`TASK_STACK_SIZE` in
`components/port_esp32/src/port_esp32.c`).

## Known pitfalls

* **C-to-C cable**: the port may not be detected. Fix: power the board off (long
  press until the green LED lights up), then reconnect the cable.
* **PSRAM is mandatory** in `sdkconfig`: without `CONFIG_SPIRAM=y` there is no
  2 MB of RAM and the `esp_psram` frame buffers will not work.
* **G25 and G36**: if one of them is used as an ADC input, the other must be
  switched to float (`gpio_pulldown_dis(GPIO_NUM_25)` / `gpio_pullup_dis`).
* **G19**: driving the IR diode turns the red LED off and vice versa; the
  drivers arbitrate the pad (`ir` takes it for a frame and gives it back),
  and the two never shine at once.
* **Display**: the Plus2 uses different DC/RST pins (G14/G12) than the StickC and
  StickC Plus (G23/G18). Code for the StickC Plus will not run on the Plus2.
* **USB-UART driver**: the Plus2 uses the CH9102 (it needs its own VCP driver),
  not CH552/FTDI as on the StickC/Plus.

## Clocks and timers

* Main clock: 240 MHz.
* Exact 32.768 kHz RTC oscillator — it is also the source of the deep-sleep
  timer and of the clock.
* ESP32 timers: two blocks of four timers each, 64 bit — used for the system tick.

## Sources

* Documentation: https://docs.m5stack.com/en/core/M5StickC%20PLUS2
* Schematic and pin map: the "PinMap" section of the documentation, PDF on
  docs.m5stack.com
* StickC / StickC Plus / StickC Plus2 comparison: the "Version Change" table in
  the documentation
