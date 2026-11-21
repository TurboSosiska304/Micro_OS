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
    "  ir         ok" \
    "  sound      ok" \
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

# The commands of P3 that do not need a person or a second machine: the frame
# of the remote is built (and sent - the emitter is on the board, the receiver
# is not), the detector listens to a quiet room, the alarm of the clock is
# written and cleared, and /etc carries the file of the model. The threshold
# and the alarm are left as the boot has them.
echo "==> the shell: ir, sound, rtc alarm, power and /etc"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 14 \
    --send "power" \
    --send "ir frame 04 08" \
    --send "ir send 04 08" \
    --send "sound" \
    --send "sound listen 1000" \
    --send "rtc alarm +60" \
    --send "rtc alarm" \
    --send "rtc alarm clear" \
    --send "cat /etc/setting" | tee -a "$LOG"
set -e

for marker in \
    "power: hold high" \
    "ir: nec 04:08, 34 pulses" \
    "ir: sent nec 04:08" \
    "sound: threshold 200" \
    "sound: 1000 ms" \
    "not fired" \
    "rtc alarm: off" \
    "hostname=stickc-plus2"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' after the P3 commands" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

# A sleep and the wake by the timer of the chip: the board goes down, comes
# back on its own, and the reason of the boot is the timer. The capture of the
# sleep carries the reset and the whole boot of the wake, so nothing of it is
# lost between two runs of the console.
echo "==> deep sleep: five seconds and the timer of the chip"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 22 --send "power sleep 5" | tee -a "$LOG"
set -e

for marker in \
    "power sleep: 5 s" \
    "rst:0x5 (DEEPSLEEP_RESET)" \
    "MicroOS shell ready"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' around the sleep" >&2
        missing=1
    fi
done

set +e
python3 "$ROOT/tools/console.py" "$PORT" 6 --send "power" | tee -a "$LOG"
set -e

if ! grep -qF "power: hold high, woke from timer" "$LOG"; then
    echo "the console has no 'power: hold high, woke from timer' after the sleep" >&2
    missing=1
fi

# The alarm of the clock wakes a sleeping board through the timer of the chip,
# because its INT drives the latch and not the wake line (hardware.md). The
# phase costs up to a minute of waiting for the next whole minute, so it is
# asked for: DEVICE_ALARM=1 tools/device-test.sh.
if [ "${DEVICE_ALARM:-0}" = "1" ]; then
    echo "==> deep sleep: until the alarm of the clock"
    set +e
    python3 "$ROOT/tools/console.py" "$PORT" 80 \
        --send "rtc alarm +30" \
        --send "power sleep alarm" | tee -a "$LOG"
    python3 "$ROOT/tools/console.py" "$PORT" 6 --send "power" | tee -a "$LOG"
    python3 "$ROOT/tools/console.py" "$PORT" 6 --send "rtc alarm clear" | tee -a "$LOG"
    set -e

    if ! grep -qF "power: the rtc alarm woke the board" "$LOG"; then
        echo "the console has no 'power: the rtc alarm woke the board'" >&2
        missing=1
    fi
fi

# The factory reset erases the volume and the store and leaves a board that
# boots, checks and speaks; it also costs the files of the user, so it is
# asked for: DEVICE_FACTORY=1 tools/device-test.sh.
if [ "${DEVICE_FACTORY:-0}" = "1" ]; then
    echo "==> factory reset: the volume, the store and the defaults"
    set +e
    python3 "$ROOT/tools/console.py" "$PORT" 22 --send "factory reset -y" | tee -a "$LOG"
    set -e

    for marker in \
        "factory reset: done, restarting" \
        "factory: 1 default file(s) of /etc written" \
        "module(s), 0 problem(s)" \
        "MicroOS shell ready"; do
        if ! grep -qF "$marker" "$LOG"; then
            echo "the console has no '$marker' after the reset" >&2
            missing=1
        fi
    done

    set +e
    python3 "$ROOT/tools/console.py" "$PORT" 6 \
        --send "cat /etc/setting" \
        --send "rtc alarm" | tee -a "$LOG"
    set -e

    if ! grep -qF "hostname=stickc-plus2" "$LOG"; then
        echo "the console has no /etc/setting after the reset" >&2
        missing=1
    fi
fi

if [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

echo "RESULT: PASS (the self test, /bin/hello, an x86-64 image, ir, sound, the alarm and the sleep)"
