# Micro OS — prebuilt firmware

This folder holds the **ready to flash** firmware of Micro OS for the M5Stack
StickC Plus2, and the same kernel for a PC. You do not need the sources, the
ESP-IDF or the toolchain to use it: download the folder, connect the board
and run the flash script.

| File | Description | Flash address |
|------|-------------|---------------|
| `MicroOS-<version>.bin` | The whole board image: bootloader, table and kernel | `0x0` |
| `bootloader.bin` | Second stage bootloader, for the tools that want the parts | `0x1000` |
| `partition-table.bin` | Partition table, for the tools that want the parts | `0x8000` |
| `micro_os.bin` | The kernel itself, for the tools that want the parts | `0x10000` |
| `micro_os.bin.sig` | The 120-byte trailer of `micro_os.bin` (magic, size, SHA-256, Ed25519) | `0x14F000` |
| `progs.bin` | The programs of `/bin`: the `.mox` files and the directory | `0x290000` |
| `MicroOS-<version>.bin.sig` | The trailer of the merged distribution image | - |
| `microos-sign.pub` | The public key of the build, in hex, for `microos_sign verify` | - |
| `MicroOS-x86_64-<version>.tar.gz` | The same kernel for QEMU: loader, kernel, `run.sh` and a readme | - |

The board checks `micro_os.bin` at the beginning of `app_main`, before the
filesystem and the shell: it reads the trailer from the last 4 KiB of the
application slot it is running, and verifies the SHA-256 and Ed25519 with the
public key compiled into the firmware. `flash.sh` writes the trailer at
`0x14F000` of `ota_0`; an update writes the trailer at the end of the other
slot (`0x28F000`). With `CONFIG_MICRO_OS_REQUIRE_SIGNATURE=y` (the default) a
missing or wrong trailer is not kept: the slot is marked invalid and the board
returns to the other one, and with `=n` the console warns and the board
continues (ADR 0020, ADR 0022). This is an electronic check of the
application, not secure boot: the ROM and the ESP-IDF bootloader have already
entered the app when it runs, and no eFuse is burned. The x86-64 loader of the
tarball verifies its kernel before it copies a byte. `checksums.txt` lists the
sums of the files above.

MicroFS and the NVS are **not** part of `flash.sh`: the data of the user and
the settings of the board survive a flash and an update.

## The layout of the flash

The whole 8 MB are used: two equal application slots of 1.25 MiB, 196 KB
for the programs of `/bin` and **5.25 MiB of MicroFS** for the user
(ADR 0022).

```
0x000000  +--------------------+
          | reserved           |  4 KB
0x001000  +--------------------+
          | bootloader         |  28 KB
0x008000  +--------------------+
          | partition table    |  4 KB
0x009000  +--------------------+
          | nvs                |  20 KB
0x00E000  +--------------------+
          | otadata            |  8 KB
0x010000  +--------------------+
          | ota_0              |  1.25 MiB
0x14F000  |   signature trailer|  4 KB
0x150000  +--------------------+
          | ota_1              |  1.25 MiB
0x28F000  |   signature trailer|  4 KB
0x290000  +--------------------+
          | progs              |  196 KB
0x2C1000  +--------------------+
          | MicroFS            |  5.25 MiB
0x800000  +--------------------+
```

## Flashing

```bash
chmod +x flash.sh
./flash.sh                  # the port is detected automatically
./flash.sh /dev/ttyACM0     # or give the port explicitly
```

The script writes the bootloader, the partition table, the kernel of `ota_0`,
its trailer and `progs.bin` at the offsets above, and erases otadata so the
board starts in `ota_0`. It needs `esptool`:

```bash
pip install esptool
```

or the one that comes with ESP-IDF:

```bash
. ~/esp/esp-idf/export.sh
./flash.sh
```

A board of 1.0 (one application slot, MicroFS at `0x210000`) is moved to this
layout once: the volume is at a new offset and its old bytes cannot be read
there. The flash keeps them and the board starts without a filesystem; run
`./flash.sh --erase-volume` (or `mkfs -y` at the shell) to format the new one.
The release notes say it: back up the files of the volume before the move.

## Updating without a cable

`ota_usb.py` writes a newer bundle into the slot that is **not** running, sets
otadata to it and resets the board, over the same USB cable and without
touching MicroFS or the NVS:

```bash
python3 ota_usb.py /dev/ttyACM0          # the bundle of this folder
python3 ota_usb.py --slot ota_1 /dev/ttyACM0
```

The bootloader asks the new image to confirm itself: the shell calls
`esp_ota_mark_app_valid_cancel_rollback()` when the kernel, the filesystem, the
self test, the doctor and the console are up. A boot that never gets there
(a panic, a watchdog, a refused signature) is marked aborted on the next
reset and the previous slot starts again. The tool verifies the trailer of the
bundle before it writes anything; `--force` is for a test of the rollback.

## The other machine

`MicroOS-x86_64-<version>.tar.gz` is the same kernel built for a bare x86-64
machine. Unpack it and run `./run.sh`: it starts QEMU with the loader and the
kernel, no board and no operating system under it. The console is COM1, and
the machine has a volume in its memory with a program in `/bin`.

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
   wake up; `/bin/hello` runs twice from the `progs` partition.
3. Runs the hardware doctor: display, buttons, buzzer, LED/IR, IMU, RTC,
   battery, WiFi, telnet, PSRAM and flash are checked one by one.
4. Starts the shell, and confirms the slot of an update.

## Notes

* The board needs to be reachable by the current user. On most distributions
  the serial device belongs to the `uucp` group:
  `sudo gpasswd -a $USER uucp` (log out and back in afterwards).
* With a USB-C to USB-C cable the port is sometimes not detected: power the
  board off (hold the button until the green LED lights up) and plug the
  cable in again.
* The binaries are built with ESP-IDF v5.3 for the ESP32-PICO-V3-02
  (8 MB flash, 2 MB PSRAM).
