#!/usr/bin/env bash
#
# Micro OS — the board half of the session of 600 seconds.
#
# It builds the firmware, flashes the board and spends ten minutes with it
# in one boot: programs from /bin, files, the display, the radio, telnet and
# the shell. A panic, a Guru Meditation, a backtrace, a reboot, a command
# that should pass and does not, or a silence of the console is a failure.
#
# Usage:
#   tools/board_soak.sh [--port /dev/ttyACM0] [--minutes 10]
#                       [--seconds N] [--no-build] [--no-flash]
#                       [--keep-log] [--verbose]
#
# The build and the flash are what `tools/device-test.sh` does; --no-build
# and --no-flash use the firmware already on the board, which is how the
# tool itself is checked with --minutes 1.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=""
MINUTES="10"
SECONDS_ARG=""
DO_BUILD=1
DO_FLASH=1
KEEP_LOG=""
VERBOSE=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --port)
            PORT="${2:?--port needs a value}"
            shift 2
            ;;
        --minutes)
            MINUTES="${2:?--minutes needs a value}"
            shift 2
            ;;
        --seconds)
            SECONDS_ARG="${2:?--seconds needs a value}"
            shift 2
            ;;
        --no-build)
            DO_BUILD=0
            shift
            ;;
        --no-flash)
            DO_FLASH=0
            shift
            ;;
        --keep-log)
            KEEP_LOG="--keep-log"
            shift
            ;;
        --verbose)
            VERBOSE="--verbose"
            shift
            ;;
        --)
            shift
            EXTRA+=("$@")
            break
            ;;
        *)
            echo "board-soak: unknown argument $1" >&2
            exit 2
            ;;
    esac
done

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

if [ "$DO_BUILD" -eq 1 ] || [ "$DO_FLASH" -eq 1 ]; then
    if [ -z "${IDF_EXPORT:-}" ] || [ ! -f "$IDF_EXPORT" ]; then
        echo "ESP-IDF not found (tried \$IDF_PATH, $ROOT/../esp-idf, ~/esp/esp-idf)" >&2
        echo "Set IDF_EXPORT to the path of the ESP-IDF export.sh" >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$IDF_EXPORT"
fi

cd "$ROOT"

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
    echo "No serial port found. Connect the board and pass --port /dev/ttyACM0" >&2
    exit 2
fi

if [ "$DO_BUILD" -eq 1 ]; then
    echo "==> building the firmware"
    idf.py build
fi

if [ "$DO_FLASH" -eq 1 ]; then
    # The board checks the signature of its application at the boot (the
    # requirement is on by default), so the flash is the signed bundle and
    # not the bare image of idf.py: the trailer at the end of the app
    # partition is what the check reads.
    echo "==> the bundle and the signature"
    BUNDLE="$(mktemp -d "${TMPDIR:-/tmp}/microos-board-soak.XXXXXX")"
    "$ROOT/tools/bundle.sh" "$ROOT/build" "$BUNDLE"
    echo "==> flashing $PORT"
    bash "$BUNDLE/flash.sh" "$PORT"
fi

echo "==> the session on $PORT"

ARGS=(--port "$PORT" --minutes "$MINUTES" $KEEP_LOG $VERBOSE)
if [ -n "$SECONDS_ARG" ]; then
    ARGS=(--port "$PORT" --seconds "$SECONDS_ARG" $KEEP_LOG $VERBOSE)
fi
if [ ${#EXTRA[@]} -gt 0 ]; then
    ARGS+=("${EXTRA[@]}")
fi

python3 "$ROOT/tools/board_soak_session.py" "${ARGS[@]}"
