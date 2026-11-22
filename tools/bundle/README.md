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
| `micro_os.bin.sig` | The 120-byte trailer of `micro_os.bin` (magic, size, SHA-256, Ed25519) | `0x20F000` |
| `MicroOS-<version>.bin.sig` | The trailer of the merged distribution image | - |
| `microos-sign.pub` | The public key of the build, in hex, for `microos_sign verify` | - |
| `MicroOS-x86_64-<version>.tar.gz` | The same kernel for QEMU: loader, kernel, `run.sh` and a readme | - |

The board checks `micro_os.bin` at the beginning of `app_main`, before the
filesystem and the shell: it reads the trailer from the last 4 KiB of the
factory partition and verifies the SHA-256 and Ed25519 with the public key
compiled into the firmware. `flash.sh` writes the trailer at `0x20F000`; the
flash layout does not change. With `CONFIG_MICRO_OS_REQUIRE_SIGNATURE=y` (the
default) a missing or wrong trailer stops the boot, and with `=n` the console
warns and the board continues (ADR 0020). This is an electronic check of the
application, not secure boot: the ROM and the ESP-IDF bootloader have already
entered the app when it runs, and no eFuse is burned. The x86-64 loader of
the tarball verifies its kernel before it copies a byte. `checksums.txt` lists
the sums of the files above.

The single image is the same bytes as the three parts at their offsets. It
covers `0x0` to the end of the application; the NVS and MicroFS live further
and are **not** touched by a flash of it, so the data of the volume survives
an update.

## The layout of the flash

The whole 8 MB of the flash are used, there is no room for an OTA update:
the kernel is the only application, and the rest of the flash belongs to the
file system.

```
0x00000  +--------------------+
         | reserved           |  4 KB
0x01000  +--------------------+
         | bootloader         |  28 KB
0x08000  +--------------------+
         | partition table    |  4 KB
0x09000  +--------------------+
         | nvs                |  20 KB
0x0E000  +--------------------+
         | unused             |  8 KB
0x10000  +--------------------+
         | application        |  2 MB
0x210000 +--------------------+
         | MicroFS            |  5.94 MB
0x800000 +--------------------+
```

## Flashing

```bash
chmod +x flash.sh
./flash.sh                  # the port is detected automatically
./flash.sh /dev/ttyACM0     # or give the port explicitly
```

The script writes `MicroOS-<version>.bin` at `0x0` when the bundle has it,
and falls back to the three parts at their offsets when it does not; in both
cases `micro_os.bin.sig` goes to `0x20F000`. It needs `esptool`:

```bash
pip install esptool
```

or the one that comes with ESP-IDF:

```bash
. ~/esp/esp-idf/export.sh
./flash.sh
```

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
   wake up.
3. Runs the hardware doctor: display, buttons, buzzer, LED/IR, IMU, RTC,
   battery, WiFi, telnet, PSRAM and flash are checked one by one.
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
