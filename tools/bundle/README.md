# Micro OS — prebuilt firmware

This folder holds the **ready to flash** firmware of Micro OS for the M5Stack
StickC Plus2. You do not need the sources, the ESP-IDF or the toolchain to
use it: download the folder, connect the board and run the flash script.

| File | Description | Flash address |
|------|-------------|---------------|
| `bootloader.bin` | Second stage bootloader | `0x1000` |
| `partition-table.bin` | Partition table | `0x8000` |
| `micro_os.bin` | The kernel itself | `0x10000` |

## The layout of the flash

The whole 8 MB of the flash are used, there is no room for an OTA update:
the kernel is the only application, and the rest of the flash belongs to the
file system.

```
0x00000  +--------------------+
         | boot loader        |  4 KB
0x01000  +--------------------+
         | partition table    |  4 KB
0x02000  +--------------------+
         | nvs                |  20 KB
0x09000  +--------------------+
         | application        |  2 MB
0x210000 +--------------------+
         | MicroFS            |  5.75 MB
0x800000 +--------------------+
```

## Flashing

```bash
chmod +x flash.sh
./flash.sh                  # the port is detected automatically
./flash.sh /dev/ttyACM0     # or give the port explicitly
```

The script needs `esptool`:

```bash
pip install esptool
```

or the one that comes with ESP-IDF:

```bash
. ~/esp/esp-idf/export.sh
./flash.sh
```

## The console

After the flash the kernel writes its boot log to the UART at 115200 baud.
The console tool lives in the sources, it is run from the root of the
repository:

```bash
python3 tools/console.py /dev/ttyACM0
```

You will see the boot log, the self test and the hardware doctor report, and
then the shell prompt `microos>`.

## What the kernel does at the boot

1. Boots the subsystems in order (scheduler, board, drivers).
2. Runs the self test: three tasks with different priorities sleep, yield and
   wake up.
3. Runs the hardware doctor: display, buttons, buzzer, LED/IR, IMU, RTC,
   battery, WiFi, PSRAM and flash are checked one by one.
4. Starts the shell.

## Notes

* The board needs to be reachable by the current user. On most distributions
  the serial device belongs to the `uucp` group:
  `sudo gpasswd -a $USER uucp` (log out and back in afterwards).
* With a USB-C to USB-C cable the port is sometimes not detected: power the
  board off (hold the button until the green LED lights up) and plug the
  cable in again.
* The binaries are built with ESP-IDF v5.3 for the ESP32-PICO-V3-02
  (8 MB flash, 2 MB PSRAM).
