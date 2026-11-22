#!/usr/bin/env bash
#
# Micro OS — flash the prebuilt firmware onto the board.
#
# Usage:
#   flash.sh [--erase-volume] [port]
#
# The port is optional, it is auto detected when it is not given.
# --erase-volume erases MicroFS: use it once when a board of 1.0 (one
# application and MicroFS at 0x210000) is moved to the layout of 1.1, whose
# new volume is at 0x450000 and cannot read the bytes of the old one.
#
# The script works with a standalone esptool as well as with the one that
# comes with ESP-IDF.
#
# The layout of the flash (8 MB, ADR 0022):
#   0x001000  boot loader
#   0x008000  partition table
#   0x009000  NVS (kept: it is not written)
#   0x00E000  otadata, erased so the board starts in ota_0
#   0x010000  ota_0, the application (1.25 MiB)
#   0x14F000  the signature trailer of ota_0 (the last 4 KiB of the slot)
#   0x150000  ota_1, the second application slot (1.25 MiB, untouched here)
#   0x28F000  the signature trailer of ota_1
#   0x290000  progs, the programs of /bin (0x31000)
#   0x2C1000  MicroFS, the rest of the flash (5.25 MiB)
#
# The application is written to ota_0 and otadata is erased: this script
# installs a board, it does not update one. tools/ota_usb.py updates the
# slot that is not running and keeps the data of the board.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ERASE_VOLUME=0
PORT=""

for argument in "$@"; do
    case "$argument" in
        --erase-volume|-y)
            ERASE_VOLUME=1
            ;;
        -*)
            echo "usage: flash.sh [--erase-volume] [port]" >&2
            exit 2
            ;;
        *)
            PORT="$argument"
            ;;
    esac
done

find_esptool() {
    # 1) the ESP-IDF python environment
    for candidate in "$HOME"/.espressif/python_env/*/bin/python; do
        if [ -x "$candidate" ] && "$candidate" -c "import esptool" >/dev/null 2>&1; then
            echo "$candidate -m esptool"
            return 0
        fi
    done

    # 2) esptool from the PATH
    if command -v esptool >/dev/null 2>&1; then
        echo "esptool"
        return 0
    fi

    if command -v esptool.py >/dev/null 2>&1; then
        echo "esptool.py"
        return 0
    fi

    return 1
}

ESPTOOL="$(find_esptool)" || {
    echo "esptool not found. Install it with:" >&2
    echo "    pip install esptool" >&2
    echo "or use the ESP-IDF: . ~/esp/esp-idf/export.sh" >&2
    exit 3
}

if [ -z "$PORT" ]; then
    PORT="$(python3 - <<'PY'
import glob

candidates: list[str] = []
for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
    candidates += sorted(glob.glob(pattern))
print(candidates[0] if candidates else "")
PY
)"
fi

if [ -z "$PORT" ]; then
    echo "No serial port found. Connect the board or pass the port:" >&2
    echo "    ./flash.sh /dev/ttyACM0" >&2
    exit 2
fi

# The addresses of partitions.csv, named once here so that a person sees the
# whole layout in one place. A bundle carries the parts; the merged image is
# for a tool or a person who wants one write at 0x0 (it also writes the NVS
# and otadata as 0xFF, which a normal flash of this script does not).
BOOTLOADER_ADDR=0x1000
TABLE_ADDR=0x8000
OTADATA_ADDR=0xE000
OTADATA_SIZE=0x2000
APP_ADDR=0x10000
APP_SIZE=0x140000
TRAILER_ADDR=0x14F000
PROGS_ADDR=0x290000
PROGS_SIZE=0x31000
MFS_ADDR=0x2C1000
MFS_SIZE=0x53F000

for name in bootloader.bin partition-table.bin micro_os.bin progs.bin; do
    if [ ! -f "$HERE/$name" ]; then
        echo "the bundle has no $name" >&2
        exit 1
    fi
done

TRAILER="$HERE/micro_os.bin.sig"
if [ -f "$TRAILER" ] && [ "$(stat -c %s "$TRAILER")" -ne 120 ]; then
    echo "the trailer $TRAILER is not 120 bytes" >&2
    exit 1
fi

APP_BYTES="$(stat -c %s "$HERE/micro_os.bin")"
APP_LIMIT=$((APP_SIZE - 0x1000))
if [ "$APP_BYTES" -gt "$APP_LIMIT" ]; then
    echo "micro_os.bin is $APP_BYTES bytes and the slot leaves $APP_LIMIT" >&2
    exit 1
fi

PROGS_BYTES="$(stat -c %s "$HERE/progs.bin")"
if [ "$PROGS_BYTES" -ne $((PROGS_SIZE)) ]; then
    echo "progs.bin is $PROGS_BYTES bytes and the partition is $PROGS_SIZE" >&2
    exit 1
fi

echo "==> erasing otadata: the board starts in ota_0"
# An old otadata may name ota_1, whose application this script does not
# write. The two sectors have to go: the bootloader then starts ota_0 and
# records it as the valid slot.
$ESPTOOL --chip esp32 -p "$PORT" -b 460800 --before default_reset --after no_reset \
    erase_region "$OTADATA_ADDR" "$OTADATA_SIZE"

if [ "$ERASE_VOLUME" -eq 1 ]; then
    echo "==> erasing MicroFS at $MFS_ADDR: the volume of the board is lost"
    $ESPTOOL --chip esp32 -p "$PORT" -b 460800 --before default_reset --after no_reset \
        erase_region "$MFS_ADDR" "$MFS_SIZE"
fi

echo "==> flashing $PORT with $ESPTOOL"
WRITES=("$BOOTLOADER_ADDR" "$HERE/bootloader.bin" \
        "$TABLE_ADDR" "$HERE/partition-table.bin" \
        "$APP_ADDR" "$HERE/micro_os.bin")
if [ -f "$TRAILER" ]; then
    WRITES+=("$TRAILER_ADDR" "$TRAILER")
fi
WRITES+=("$PROGS_ADDR" "$HERE/progs.bin")

# The five writes are one session of esptool: the offsets are the layout
# above and the NVS at 0x9000 is not among them. The trailer of the signature
# of an older image in ota_1 is left alone; that slot is not booted until an
# update writes it.
$ESPTOOL --chip esp32 -p "$PORT" -b 460800 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 40m --flash_size 8MB \
    "${WRITES[@]}"

echo "==> done. Start the console with:"
echo "    python3 tools/console.py $PORT"
echo
echo "The layout is the one of 1.1 (ADR 0022). A board of 1.0 loses its volume"
echo "once when this image is written: the new MicroFS is at $MFS_ADDR, and the"
echo "old one cannot be read there. Use --erase-volume to format it in the same"
echo "pass, or run 'mkfs -y' at the shell of the board."
