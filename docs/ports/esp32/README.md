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

## The signature of the image

The application checks itself at the beginning of `app_main`, before MicroFS
and the shell (ADR 0020). The slot it is running (ADR 0022: `ota_0` or
`ota_1`, from `esp_ota_get_running_partition()`) is read in place: the ESP
header gives the length of the image, `esp_app_desc_t` at `0x20` gives its
version, and the last 4 KiB of the slot (`0x12F000` or `0x24F000`) hold the
120-byte `MOSIGN01` trailer over the bytes of `micro_os.bin`. The digest is
SHA-256 over the flash in chunks and the signature is Ed25519 with the public
key compiled into the firmware.

`tools/bundle.sh` signs the application and the merged distribution image;
`micro_os.bin.sig` is the trailer, `flash.sh` writes it at `0x12F000` of
`ota_0`, and `tools/ota_usb.py` writes it at the end of the slot it updates.
`make bundle` also checks that the image and its 4 KiB reserve fit a 1.125 MiB
slot; the ESP-IDF build checks the same before it writes the image.

A signature that does not pass with the policy on is not kept: the running
slot is marked invalid and the bootloader returns the board to the other one
(see "Updates and rollback"). With no other image to run the board stops.

The policy is a Kconfig setting:

* `CONFIG_MICRO_OS_REQUIRE_SIGNATURE` (default `y`): a missing or wrong
  trailer prints `boot: the image is refused: <reason>`, marks the slot
  invalid and restarts into the other slot, or stops when there is none. With
  `n`, the same check prints
  `boot: unsigned or damaged image: not guaranteed` and the boot continues.
* `CONFIG_MICRO_OS_ANTIROLLBACK` (default `n`): the newest version seen is
  kept in the NVS namespace `microos_boot` (`min_version`). With the flag on,
  an image older than the stored one is refused; with it off, it is a
  warning. A newer version is recorded after the signature passed. This is a
  version floor, not the A/B rollback of the next section.

The check is electronic, not secure boot: the ROM and the ESP-IDF bootloader
have already entered the application, so a damaged image may have run its
first instructions before the sentence. It cannot stop a local attacker who
can rewrite the flash or the NVS. Secure boot v2 and flash encryption are not
enabled by this tree.

`tools/device-test.sh` runs the signed bundle by default. Two more policies
are available for a development board:

    DEVICE_POLICY=unsigned tools/device-test.sh /dev/ttyACM0
    DEVICE_POLICY=require-unsigned tools/device-test.sh /dev/ttyACM0

The first builds with the requirement off and flashes an image without a
trailer: the console warns and the shell comes up. The second keeps the
requirement on and flashes the same unsigned image: the boot refuses it and
the board returns to the slot it came from (or stops when it has none).

## Updates and rollback

The flash has two equal application slots (ADR 0022). `flash.sh` installs into
`ota_0` and erases otadata; `tools/ota_usb.py` (in the bundle, and in
`tools/`) updates the slot that is **not** running:

```bash
python3 ota_usb.py /dev/ttyACM0
```

The tool reads otadata, verifies the trailer of the bundle when `microos_sign`
is beside it, writes the application and its trailer into the inactive slot,
sets the state `NEW` in otadata and resets the board. The bootloader changes
the state to `PENDING_VERIFY` and starts the new image; `app_main` checks the
signature, and the shell task calls
`esp_ota_mark_app_valid_cancel_rollback()` when the kernel, the filesystem,
the programs, the self test, the doctor and the console are up. A boot that
ends before that line (a panic, the task watchdog, a halt, a power cut) is
marked `ABORTED` on the next reset and the previous slot starts again. A
refused signature does the same at once with
`esp_ota_mark_app_invalid_rollback_and_reboot()`.

`DEVICE_OTA=1 tools/device-test.sh /dev/ttyACM0` proves both directions on the
board: an update to `ota_1`, an update back to `ota_0`, and an image with a
wrong trailer that is refused and rolls back to `ota_0`.

The update writes only the inactive slot and otadata: MicroFS, the NVS and
the bootloader are not touched. **The update over the network is not in 1.1**
(ADR 0022): the HTTP client of the kernel is not a stream for
`esp_ota_write()`, and the cable is the update path of this release.

## The programs of /bin

`progs` is a partition of 132 KiB (ADR 0018 §2, ADR 0022). Its last 4 KiB
holds a directory of name, offset and size, and the two 64 KiB slots before
it start at offset 0 and 0x10000: every program is on a 64 KiB boundary, and
`hello` and `cat` fill the two slots. `tools/progs_image.py` builds the
image from the `.mox` files of the build, `idf.py flash` writes it with the
firmware, and the bundle carries it as `progs.bin`. The port mounts it at
`/bin` as a read-only filesystem: `ls /bin`, `cat /bin/hello` and
`exec /bin/hello` are ordinary VFS calls, and no image of a program is
embedded in the firmware. A new program is a new `progs.bin` (with the same
two names or another pair), not a rebuild of the kernel.

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
ir         ok        NEC tx on G19, shared with the led; no rx
battery    ok        4176 mV, 98% (raw 2088 mV)
mic        ok        16000 Hz, rms 20, peak 60
sound      ok        quiet: rms 20, peak 60, threshold 200
power      ok        hold on, wake power-on
telnet     ok        port 23, no auth
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
* **A program runs, without protection.** An image of this machine (`arch` 1,
  Xtensa) is entered by the port (ADR 0018): the text in 8 KiB of instruction
  RAM at `0x4009E000`, the data and the bss in 8 KiB of DRAM at
  `0x3FFCE000`, one program at a time. There is no MMU and no address space:
  a program shares the memory of the kernel, a bad pointer can panic and
  reboot the board, and a bigger image is `ENOMEM`. The programs are files of
  the `progs` partition, mounted read-only at `/bin` (ADR 0022); a new program
  is a new `progs.bin` and a flash, not a file to copy at the shell.
* **8 MB of flash: two slots and 5.56 MiB of MicroFS.** `ota_0` and `ota_1`
  are 1.125 MiB each, `progs` is 132 KiB and MicroFS is the rest
  ([partitions.csv](../../../partitions.csv), ADR 0022). A board of 1.0 loses
  its volume once when the new table is written: the volume moves from
  `0x210000` to `0x271000` and its old bytes are not a volume there.
* **The update is over the cable.** `tools/ota_usb.py` writes the inactive
  slot and the rollback returns to the old image when the new one does not
  reach the shell. There is no update over the network in 1.1 (ADR 0022).
* **No PMIC.** Power and sleep belong to the ESP32 itself: `HOLD` (G4) must be
  high after a wake-up or the board powers off. The alarm of the clock wakes a
  sleeping board only because the firmware sets the timer of the chip for its
  moment — the INT of the BM8563 is not on the wake line.
* **The IR pin is shared with the red LED** (G19): the drivers arbitrate it,
  and the two never shine at once.
* **There is no IR receiver on the board.** `ir send` and `ir frame` work by
  themselves; `ir rx` waits for a demodulating element a person wires to G26.
* **The battery percent is a curve, not a fuel gauge.** The charge is looked
  up from the voltage of the cell; while the cable is connected the divider
  sees the charging voltage and the percent sits at the top.
* **One power button and two user buttons** (A, B), plus C on the Plus2.
