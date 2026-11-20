# The ESP32 port

The board is one port of the kernel: `components/port_esp32` implements the
contract of `kernel/port/*.h`, `components/board` describes the machine, and
`components/drivers` talks to the chips.

| | |
|---|---|
| board | M5Stack StickC Plus2, ESP32-PICO-V3-02, 240 MHz, 2 MB PSRAM, 8 MB flash |
| display | ST7789v2, 135x240, the console of the system |
| build | `make idf-build` (ESP-IDF v5.3) |
| on the board | `tools/device-test.sh /dev/ttyACM0` |
| plan | [roadmap.md](roadmap.md) |
| pins and quirks | [hardware.md](hardware.md) |

What the port answers that the others do not: a program cannot be run from
the flash (`k_port_exec_supported()` is false - an arbitrary file is not
executable there, and the internal SRAM has about 100 KB), and the store of
the configuration is the NVS of the chip.
