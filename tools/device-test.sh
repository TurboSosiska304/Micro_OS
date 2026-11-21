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
PORT="${1:-}"
CONSOLE_SECONDS="${CONSOLE_SECONDS:-30}"

# ESP-IDF discovery, in order: $IDF_PATH, a checkout beside the repository
# (../esp-idf), then the conventional ~/esp/esp-idf. IDF_EXPORT overrides.
if [ -z "${IDF_EXPORT:-}" ]; then
    for candidate in "${IDF_PATH:+$IDF_PATH/export.sh}" \
                     "$ROOT/../esp-idf/export.sh" \
                     "$HOME/esp/esp-idf/export.sh"; do
        if [ -n "$candidate" ] && [ -f "$candidate" ]; then
            IDF_EXPORT="$candidate"
            break
        fi
    done
fi

if [ -z "${IDF_EXPORT:-}" ] || [ ! -f "$IDF_EXPORT" ]; then
    echo "ESP-IDF not found (tried \$IDF_PATH, $ROOT/../esp-idf, ~/esp/esp-idf)" >&2
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
LOG="$(mktemp "${TMPDIR:-/tmp}/microos-console.XXXXXX")"
trap 'rm -f "$LOG"' EXIT

set +e
python3 "$ROOT/tools/console.py" "$PORT" "$CONSOLE_SECONDS" | tee "$LOG"
console_status=${PIPESTATUS[0]}
set -e

# The program of the board runs at the boot (the self test), so the console
# has to carry its lines: the PROGRAM marker and the exit status. A board that
# boots without them is a failing device test even when the rest of the self
# test passed.
missing=0
for marker in \
    "PROGRAM: hello from /bin" \
    "PROGRAM: arch xtensa, no protection" \
    "PROGRAM: argc 3" \
    "PROGRAM: envp 4" \
    "PROGRAM: exit 0" \
    "module(s), 0 problem(s)"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker'" >&2
        missing=1
    fi
done

if [ "$console_status" -ne 0 ] || [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

# The shell is up at the end of the capture. Two lines prove the other half of
# the door: the same file from the command line, and an image of the other
# machine, which has to be ENOEXEC before a byte of it runs.
echo "==> the shell: exec /bin/hello and an image of x86-64"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 10 \
    --send "exec /bin/hello one two" \
    --send "exec /bin/foreign" | tee -a "$LOG"
set -e

for marker in \
    "PROGRAM: argv[1] one" \
    "the program has finished" \
    "exec: /bin/foreign: exec format error"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' after the shell commands" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

echo "RESULT: PASS (the self test, /bin/hello and an x86-64 image on the board)"
