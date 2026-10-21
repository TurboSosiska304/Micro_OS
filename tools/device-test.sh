#!/usr/bin/env bash
#
# Micro OS — build, flash and check the firmware on a real device.
#
# Usage:
#   tools/device-test.sh [port]
#
# The port is optional, it is auto detected when it is not given.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
PORT="${1:-}"
CONSOLE_SECONDS="${CONSOLE_SECONDS:-30}"

if [ ! -f "$IDF_EXPORT" ]; then
    echo "ESP-IDF not found at $IDF_EXPORT" >&2
    echo "Set IDF_EXPORT to the path of the ESP-IDF export.sh" >&2
    exit 1
fi

# shellcheck disable=SC1090
. "$IDF_EXPORT"

cd "$ROOT"

echo "==> building the firmware"
idf.py build

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
    echo "No serial port found. Connect the board and pass the port explicitly:" >&2
    echo "    tools/device-test.sh /dev/ttyACM0" >&2
    exit 2
fi

echo "==> flashing $PORT"
idf.py -p "$PORT" flash

echo "==> reading the console"
python3 "$ROOT/tools/console.py" "$PORT" "$CONSOLE_SECONDS"
