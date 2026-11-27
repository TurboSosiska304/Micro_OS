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
# The policy of the image under test (ADR 0020):
#   signed           the normal build, the bundle, the trailer in the flash
#   unsigned         CONFIG_MICRO_OS_REQUIRE_SIGNATURE=n, no trailer: warning
#   require-unsigned the normal build, no trailer: the boot stops
DEVICE_POLICY="${DEVICE_POLICY:-signed}"
BUILD_DIR="${IDF_BUILD_DIR:-$ROOT/build}"
UNSIGNED_BUILD="${DEVICE_UNSIGNED_BUILD:-$ROOT/build-unsigned}"

case "$DEVICE_POLICY" in
    signed|unsigned|require-unsigned) ;;
    *)
        echo "DEVICE_POLICY must be signed, unsigned or require-unsigned" >&2
        exit 2
        ;;
esac

# The secret of the release is the key material (hex), not a path: the tool
# and the build want a file, so the material is written where they can read
# it. A value that is already a path is left alone.
if [ -n "${MICROOS_SIGN_KEY:-}" ] && [ ! -f "$MICROOS_SIGN_KEY" ]; then
    KEY_FILE="$(mktemp "${TMPDIR:-/tmp}/microos-key.XXXXXX")"
    chmod 600 "$KEY_FILE"
    printf '%s\n' "$MICROOS_SIGN_KEY" > "$KEY_FILE"
    export MICROOS_SIGN_KEY="$KEY_FILE"
fi

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

if [ "$DEVICE_POLICY" = "unsigned" ] || [ "$DEVICE_POLICY" = "require-unsigned" ]; then
    if [ "$DEVICE_POLICY" = "unsigned" ]; then
        # A build of its own with the requirement off. The defaults are the
        # normal file plus the one line; the sdkconfig is generated, so a
        # stale one is removed and the defaults win.
        UNSIGNED_DEFAULTS="$(mktemp "${TMPDIR:-/tmp}/microos-unsigned.XXXXXX")"
        UNSIGNED_SDKCONFIG="$UNSIGNED_BUILD/sdkconfig"
        cat "$ROOT/sdkconfig.defaults" > "$UNSIGNED_DEFAULTS"
        printf '\n# CONFIG_MICRO_OS_REQUIRE_SIGNATURE is not set\n' >> "$UNSIGNED_DEFAULTS"
        rm -f "$UNSIGNED_SDKCONFIG"
        echo "==> building the firmware without the signature requirement"
        idf.py -B "$UNSIGNED_BUILD" \
            -D SDKCONFIG="$UNSIGNED_SDKCONFIG" \
            -D SDKCONFIG_DEFAULTS="$UNSIGNED_DEFAULTS" build
    else
        echo "==> building the firmware (the signature is required)"
        idf.py -B "$BUILD_DIR" build
    fi
else
    echo "==> building the firmware"
    idf.py -B "$BUILD_DIR" build
fi

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

if [ "$DEVICE_POLICY" = "signed" ]; then
    echo "==> the bundle and the signature"
    BUNDLE="$(mktemp -d "${TMPDIR:-/tmp}/microos-bundle.XXXXXX")"
    trap 'rm -rf "$BUNDLE"' EXIT
    "$ROOT/tools/bundle.sh" "$BUILD_DIR" "$BUNDLE"
    echo "==> flashing $PORT (the parts, otadata erased, the trailer at 0x12F000)"
    "$BUNDLE/flash.sh" "$PORT"
else
    # Erase the 4 KiB where the trailer lives and otadata, so the raw image
    # of the unsigned policies is the one the board starts: a valid trailer
    # of an earlier signed flash must not be inherited, and an otadata that
    # names the other slot must not hide the image just written.
    NO_TRAILER="$(mktemp "${TMPDIR:-/tmp}/microos-notrailer.XXXXXX")"
    python3 -c 'import sys; open(sys.argv[1], "wb").write(b"\xff" * 4096)' "$NO_TRAILER"
    trap 'rm -f "$NO_TRAILER"' EXIT
    if [ "$DEVICE_POLICY" = "unsigned" ]; then
        echo "==> flashing $PORT without a trailer"
        idf.py -B "$UNSIGNED_BUILD" -p "$PORT" flash
    else
        echo "==> flashing $PORT without a trailer"
        idf.py -B "$BUILD_DIR" -p "$PORT" flash
    fi
    python3 -m esptool --chip esp32 -p "$PORT" write_flash 0x12F000 "$NO_TRAILER"
    python3 -m esptool --chip esp32 -p "$PORT" erase_region 0xE000 0x2000
fi

echo "==> reading the console"
LOG="$(mktemp "${TMPDIR:-/tmp}/microos-console.XXXXXX")"
trap 'rm -f "$LOG"; if [ -n "${UNSIGNED_DEFAULTS:-}" ]; then rm -f "$UNSIGNED_DEFAULTS"; fi; if [ -n "${BUNDLE:-}" ]; then rm -rf "$BUNDLE"; fi; if [ -n "${NO_TRAILER:-}" ]; then rm -f "$NO_TRAILER"; fi' EXIT

set +e
python3 "$ROOT/tools/console.py" "$PORT" "$CONSOLE_SECONDS" | tee "$LOG"
console_status=${PIPESTATUS[0]}
set -e

# The boot that has to stop: the line of the refusal and no shell, and no
# self test or program after it. There is no reason to run the rest of the
# session, because this image is not supposed to boot.
if [ "$DEVICE_POLICY" = "require-unsigned" ]; then
    if grep -qF "the image is refused" "$LOG" &&
       ! grep -qF "MicroOS shell ready" "$LOG" &&
       ! grep -qF "PROGRAM: hello from /bin" "$LOG"; then
        echo "RESULT: PASS (the unsigned image is refused before the shell)"
        exit 0
    fi
    echo "the console does not show a stopped unsigned image" >&2
    echo "RESULT: FAIL" >&2
    exit 1
fi

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
    "boot: running from ota_0" \
    "boot: the image of ota_0 is valid, the rollback is cancelled" \
    "there is no append volume, /etc/bootlog.log and /etc/run.log are in RAM" \
    "  ir         ok" \
    "  sound      ok" \
    "module(s), 0 problem(s)"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker'" >&2
        missing=1
    fi
done

# The line of the policy: the signed bundle says the signature was checked,
# the unsigned build says what its boot costs.
if [ "$DEVICE_POLICY" = "signed" ] && ! grep -qF "image signature ok (sha-256, ed25519)" "$LOG"; then
    echo "the console does not show the image signature" >&2
    missing=1
fi
if [ "$DEVICE_POLICY" = "unsigned" ] &&
   ! grep -qF "unsigned or damaged image: not guaranteed" "$LOG"; then
    echo "the console does not warn about the unsigned image" >&2
    missing=1
fi

if [ "$console_status" -ne 0 ] || [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

# The shell is up at the end of the capture. Three lines prove the other half
# of the door: /bin is the list of the programs of the partition, the same
# file runs from the command line, /etc has no file logs on the board, and an
# image of the other machine is ENOEXEC before a byte of it runs.
echo "==> the shell: ls /bin, exec /bin/hello and an image of x86-64"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 12 \
    --send "ls /bin" \
    --send "mount" \
    --send "exec /bin/hello one two" \
    --send "ls /etc" \
    --send "exec /log/.foreign" | tee -a "$LOG"
set -e

for marker in \
    "hello" \
    "cat" \
    "PROGRAM: argv[1] one" \
    "the program has finished" \
    " on /bin type " \
    "exec: /log/.foreign: exec format error"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' after the shell commands" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
fi

# The update over USB (ADR 0022): the image goes into the slot that is not
# running, otadata selects it, the bootloader asks it to confirm itself, and
# the shell confirms it. Three writes prove both halves and the rollback: a
# good image into ota_1, a good image back into ota_0, and an image whose
# trailer does not match, which the boot must refuse and give back to the
# other slot. It writes the flash a few times, so it is asked for:
# DEVICE_OTA=1 tools/device-test.sh /dev/ttyACM0.
if [ "${DEVICE_OTA:-0}" = "1" ]; then
    if [ "$DEVICE_POLICY" != "signed" ]; then
        echo "DEVICE_OTA needs DEVICE_POLICY=signed (the bundle and its trailer)" >&2
        exit 2
    fi

    echo "==> update over USB: ota_1"
    set +e
    python3 "$ROOT/tools/ota_usb.py" --port "$PORT" --dir "$BUNDLE" | tee -a "$LOG"
    python3 "$ROOT/tools/console.py" "$PORT" 45 | tee -a "$LOG"
    set -e

    for marker in \
        "the image goes to ota_1" \
        "boot: running from ota_1" \
        "image signature ok (sha-256, ed25519), ota_1" \
        "boot: the image of ota_1 is valid, the rollback is cancelled" \
        "MicroOS shell ready"; do
        if ! grep -qF "$marker" "$LOG"; then
            echo "the console has no '$marker' after the update to ota_1" >&2
            missing=1
        fi
    done

    echo "==> update over USB: back to ota_0"
    set +e
    python3 "$ROOT/tools/ota_usb.py" --port "$PORT" --dir "$BUNDLE" | tee -a "$LOG"
    python3 "$ROOT/tools/console.py" "$PORT" 45 | tee -a "$LOG"
    set -e

    for marker in \
        "the image goes to ota_0" \
        "boot: running from ota_0" \
        "boot: the image of ota_0 is valid, the rollback is cancelled"; do
        if ! grep -qF "$marker" "$LOG"; then
            echo "the console has no '$marker' after the update to ota_0" >&2
            missing=1
        fi
    done

    # A trailer that does not match its image: the app refuses it at the boot
    # and the board returns to the slot it came from. The host tool would
    # refuse to write it, so --force is the test of the rollback itself.
    echo "==> update over USB: an image with a wrong trailer rolls back"
    BAD_BUNDLE="$(mktemp -d "${TMPDIR:-/tmp}/microos-badbundle.XXXXXX")"
    cp -f "$BUNDLE/micro_os.bin" "$BUNDLE/partition-table.bin" "$BAD_BUNDLE/"
    if [ -f "$BUNDLE/microos-sign.pub" ]; then
        cp -f "$BUNDLE/microos-sign.pub" "$BAD_BUNDLE/"
    fi
    python3 - "$BUNDLE/micro_os.bin.sig" "$BAD_BUNDLE/micro_os.bin.sig" <<'PY'
import sys

data = bytearray(open(sys.argv[1], "rb").read())
data[-1] ^= 0xFF
open(sys.argv[2], "wb").write(data)
PY
    set +e
    python3 "$ROOT/tools/ota_usb.py" --port "$PORT" --dir "$BAD_BUNDLE" --force | tee -a "$LOG"
    rm -rf "$BAD_BUNDLE"
    python3 "$ROOT/tools/console.py" "$PORT" 60 | tee -a "$LOG"
    set -e

    for marker in \
        "the image is refused" \
        "boot: running from ota_0" \
        "MicroOS shell ready"; do
        if ! grep -qF "$marker" "$LOG"; then
            echo "the console has no '$marker' after the wrong trailer" >&2
            missing=1
        fi
    done
fi

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

# The settings of the machine (ADR 0021): the systems of /etc are read at the
# boot, `config set` writes the file and applies it at once, and `config sync`
# puts the text in the NVS, so the reboot below brings it back. The turn and
# the scale are asked for and then given back to the defaults: the next run of
# this test starts from the picture of the release.
echo "==> the settings of /etc: the screen, the font and the store"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 16 \
    --send "config list" \
    --send "config get display.rotation" \
    --send "config set display.rotation 90" \
    --send "config set font.scale 2" \
    --send "config get font.scale" \
    --send "config sync" | tee -a "$LOG"
set -e

for marker in \
    "display  /etc/display" \
    "display.rotation=0" \
    "display: rotation 90" \
    "font: scale 2" \
    "display.rotation=90" \
    "font.scale=2"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' after the settings commands" >&2
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

# The reboot took the RAM away and the NVS kept the text of the systems: the
# two values that `config sync` saved before the sleep are the ones the board
# boots with, and the log of the boot says the settings were applied.
echo "==> the settings after the reboot, and the defaults again"
set +e
python3 "$ROOT/tools/console.py" "$PORT" 8 \
    --send "config get display.rotation" \
    --send "config get font.scale" \
    --send "config set display.rotation 0" \
    --send "config set font.scale 1" \
    --send "config sync" | tee -a "$LOG"
set -e

for marker in "display.rotation=90" "font.scale=2" "display.rotation=0" "font.scale=1"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "the console has no '$marker' after the reboot" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "RESULT: FAIL" >&2
    exit 1
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
