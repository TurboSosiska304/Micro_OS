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
MONITOR_SECONDS="${MONITOR_SECONDS:-20}"

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
    PORT="$(python3 - "$HOME" <<'PY'
import glob
import os
import sys

home = sys.argv[1]
candidates = []
for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*"):
    candidates += sorted(glob.glob(pattern))
print(candidates[0] if candidates else "")
PY
)"
fi

if [ -z "$PORT" ]; then
    echo "No serial port found. Connect the board and pass the port explicitly:" >&2
    echo "    tools/device-test.sh /dev/ttyUSB0" >&2
    exit 2
fi

echo "==> flashing $PORT"
idf.py -p "$PORT" flash

echo "==> capturing the console for ${MONITOR_SECONDS}s"
LOG="$(mktemp)"

# idf.py monitor does not terminate on its own, so it is run in the
# background and stopped after the timeout.
timeout "$MONITOR_SECONDS" idf.py -p "$PORT" monitor > "$LOG" 2>&1 || true

cat "$LOG"

if grep -q "selftest: PASS" "$LOG"; then
    echo "==> RESULT: PASS"
    rm -f "$LOG"
    exit 0
fi

echo "==> RESULT: FAIL (no 'selftest: PASS' in the output)" >&2
echo "    the log is kept at $LOG" >&2
exit 1
