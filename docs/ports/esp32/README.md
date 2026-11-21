# The ESP32 port: the board

One port of the kernel: the M5Stack StickC Plus2 (ESP32-PICO-V3-02, 240 MHz,
2 MB PSRAM, 8 MB flash, 135x240 TFT). Everything about this machine lives
here: its build, its checks, its limits and its plan.

| | |
|---|---|
| port | `components/port_esp32` (ESP-IDF, FreeRTOS, lwIP, NVS) |
| board | `components/board`, the drivers in `components/drivers` |
| plan | [roadmap.md](roadmap.md) (P0-P5) |
| pins, chips, quirks | [hardware.md](hardware.md) |
| build | `make idf-build` (ESP-IDF v5.3) |
| check on the board | `tools/device-test.sh /dev/ttyACM0` |
| flash | `make bundle` → `dist/flash.sh`, or `make idf-flash` |

## Building and flashing

```bash
make idf-build                  # the firmware
tools/device-test.sh /dev/ttyACM0   # build, flash, read the console
make bundle                     # dist/: the images, their sums, flash.sh
```

`make idf-build` finds ESP-IDF by itself: `$IDF_PATH`, then a checkout beside
the repository (`../esp-idf`), then `~/esp/esp-idf`. The board is on
`/dev/ttyACM0` and belongs to the group `uucp`.

The ready images also go to the releases: download the bundle, connect the
board, run `bash flash.sh` (or `chmod +x flash.sh && ./flash.sh`) - no
sources, no ESP-IDF, no toolchain.

## The hardware doctor

The init process walks through the registered modules after the self test and
prints a report; the command `modules` repeats it at any time:

```
module     state     detail
psram      ok        2048 KB
flash      ok        8 MB
imu        ok        acc -16,-2952,16728 (raw)
rtc        degraded  01:00:00 (no clock)
display    ok        135x240, fb 63 KB
input      ok        A=up B=up C=up
buzzer     ok        passive, 2 kHz via LEDC
led/ir     ok        G19, shared with the IR emitter
battery    ok        4176 mV, 98% (raw 2088 mV)
wifi       degraded  f4:65:0b:32:00:c0 idle
telnet     ok        port 23, no auth
mic        ok        16000 Hz, rms 4, peak 17
```

`ok` works, `degraded` works with a limitation, `fail` does not work, `skip`
is not implemented yet.

## The console on the panel

The output of the shell is mirrored to the TFT: the renderer of the panel
(`drv_display`) draws what the console of the kernel gives it, and the console
notices a turned panel by asking the renderer for its size. `display` is the
command of the panel: `display rotate <0|90|180|270>`, `display <x> <y>` for
the offset of the glass, `display invert`, `display draw [seconds]` for the
test picture.

## Limits of this port

* **No MMU.** The separation of user and kernel space is logical; there is no
  memory protection. `fork()` shares the memory of the parent and re-enters
  its program.
* **A program cannot be run from the flash yet.** The header of an image
  names this machine (`arch` 1, Xtensa) and the port answers with it, but an
  arbitrary file is not executable there and the internal SRAM has about
  100 KB: `k_port_exec_supported()` is false, an Xtensa image is `ENOSYS` and
  an image of another machine is `ENOEXEC` (ADR 0018).
* **8 MB of flash, one application slot.** `factory` 2 MB and MicroFS 5.75 MB;
  no room for an OTA image, and no OTA ([partitions.csv](../../../partitions.csv)).
* **No PMIC.** Power and sleep belong to the ESP32 itself: `HOLD` (G4) must be
  high after a wake-up or the board powers off.
* **The battery percent is a curve, not a fuel gauge.** The charge is looked
  up from the voltage of the cell; while the cable is connected the divider
  sees the charging voltage and the percent sits at the top.
* **One power button and two user buttons** (A, B), plus C on the Plus2.
