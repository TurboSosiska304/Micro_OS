#!/usr/bin/env bash
#
# Micro OS — flash the prebuilt firmware onto the board.
#
# Usage:
#   bild/flash.sh [port]
#
# The port is optional, it is auto detected when it is not given.
#
# The script works with a standalone esptool as well as with the one that
# comes with ESP-IDF.
#
# The layout of the flash (8 MB):
#   0x0000  boot loader
#   0x8000  partition table
#   0x10000 the application (2 MB)
#   0x21000 MicroFS, the rest of the flash (5.75 MB)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-}"

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
    echo "    bild/flash.sh /dev/ttyACM0" >&2
    exit 2
fi

echo "==> flashing $PORT with $ESPTOOL"

$ESPTOOL --chip esp32 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash \
    0x1000 "$HERE/bootloader.bin" \
    0x8000 "$HERE/partition-table.bin" \
    0x10000 "$HERE/micro_os.bin"

echo "==> done. Start the console with:"
echo "    python3 tools/console.py $PORT"
