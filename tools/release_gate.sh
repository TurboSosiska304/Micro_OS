#!/bin/sh
# Micro OS — the gate of a release, in one command.
#
# The list is `docs/plan-1.8.md` §"The gate of the release": everything the
# previous release proved, the gates of the lanes of the new one, the two
# sessions of 600 seconds, the board, a Release build from an empty directory
# and the integrity of the repository. A release is a promise made of numbers,
# and this script is the list of them: it runs every gate, keeps going after a
# failure so that one red line does not hide the other nine, and writes the
# table the notes of the release are filled from.
#
# Usage:
#   tools/release_gate.sh [--tag vX.Y.Z] [--quick] [--no-board] [--log DIR]
#
#   --tag      the tag the release will be; the integrity check names it
#              (default: the tag of HEAD, or none)
#   --quick    the short smokes only: no 600-second session, no board
#   --no-board the board of the ESP32 is not on the serial port of this machine
#   --log      where the logs go (default: /tmp/opencode/release-gate)
#
# A gate that fails does not stop the run: the exit code is the number of the
# gates that failed, and every line of the table says its own answer.
set -u

cd "$(dirname "$0")/.." || exit 1

TAG=""
QUICK=0
BOARD=1
LOGDIR="/tmp/opencode/release-gate"
SERIAL="${MICROOS_SERIAL:-/dev/ttyACM0}"

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="$2"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --no-board) BOARD=0; shift ;;
        --log) LOGDIR="$2"; shift 2 ;;
        --serial) SERIAL="$2"; shift 2 ;;
        *) echo "release-gate: unknown argument $1" >&2; exit 2 ;;
    esac
done

if [ -z "$TAG" ]; then
    TAG="$(git describe --tags --exact-match 2>/dev/null || true)"
fi

mkdir -p "$LOGDIR" || exit 1
: > "$LOGDIR/table.txt"
FAILED=0
TOTAL=0

# The firmware of the board needs its environment, and the Makefile finds one
# on its own; this is the same door for the shell of the script.
if [ -z "${IDF_EXPORT:-}" ] && [ -f ../esp-idf/export.sh ]; then
    IDF_EXPORT="../esp-idf/export.sh"
fi
export IDF_EXPORT="${IDF_EXPORT:-}"

# The space of a gate: a full /tmp turns six hundred checks of a volume into a
# failure that says nothing about the code (the row of docs/debts.md).
FREE_MB="$(df -Pm /tmp | awk 'NR == 2 { print $4 }')"
if [ "${FREE_MB:-0}" -lt 2048 ]; then
    echo "release-gate: /tmp has ${FREE_MB} MB free, the images of the gates need 2 GB" >&2
    exit 2
fi

run() {
    name="$1"
    shift
    TOTAL=$((TOTAL + 1))
    log="$LOGDIR/$(echo "$name" | tr ' /' '__').log"
    printf '%-26s ' "$name"
    if "$@" > "$log" 2>&1; then
        detail="$(sed -n 's/.*OK: \([0-9]* check(s)\) passed.*/\1/p' "$log" | tail -1)"
        printf 'PASS  %s\n' "${detail:-}"
        printf '| `%s` | PASS %s |\n' "$name" "${detail:-(the build answered)}" >> "$LOGDIR/table.txt"
    else
        FAILED=$((FAILED + 1))
        printf 'FAIL  %s\n' "$log"
        printf '| `%s` | FAIL (%s) |\n' "$name" "$log" >> "$LOGDIR/table.txt"
    fi
}

# A command of make, with the parallelism of a quiet machine: three, which is
# what every lane and the cloud use.
m() {
    CMAKE_BUILD_PARALLEL_LEVEL=3 make "$@"
}

# --- the host ---------------------------------------------------------------
run "unit tests" m test
run "asan" m test-asan
run "tsan" m test-tsan
run "minimal" m test-minimal
run "two cores" m test-smp
run "valgrind" m test-valgrind
run "simulator" m smoke

# --- the machine of x86-64 --------------------------------------------------
run "x86-smoke" m x86-smoke
run "x86-vga-smoke" m x86-vga-smoke
run "x86-disk-smoke" m x86-disk-smoke
run "x86-root-smoke" m x86-root-smoke
run "x86-volume-smoke" m x86-volume-smoke
run "x86-smp-smoke" m x86-smp-smoke
run "x86-net-smoke" m x86-net-smoke
run "usb-smoke" m usb-smoke
run "ports-smoke" m ports-smoke
run "ahci-smoke" m ahci-smoke
run "nvme-smoke" m nvme-smoke
run "uefi-smoke" m uefi-smoke
run "x86-iso-smoke" m x86-iso-smoke

# --- the firmware -----------------------------------------------------------
run "idf-build" m idf-build

# --- the sessions -----------------------------------------------------------
if [ "$QUICK" -eq 0 ]; then
    run "soak 600" m soak
    run "soak-smp 600" m soak-smp
fi

# --- the board --------------------------------------------------------------
if [ "$BOARD" -eq 1 ] && [ "$QUICK" -eq 0 ]; then
    if [ -e "$SERIAL" ]; then
        TOTAL=$((TOTAL + 1))
        printf '%-26s ' "board"
        if tools/device-test.sh "$SERIAL" > "$LOGDIR/board.log" 2>&1; then
            printf 'PASS\n'
            printf '| `board device-test` | PASS |\n' >> "$LOGDIR/table.txt"
        else
            FAILED=$((FAILED + 1))
            printf 'FAIL  %s\n' "$LOGDIR/board.log"
            printf '| `board device-test` | FAIL (%s) |\n' "$LOGDIR/board.log" >> "$LOGDIR/table.txt"
        fi
    else
        echo "board                      SKIP  ($SERIAL is not there)"
        printf '| `board device-test` | SKIP (%s is not there) |\n' "$SERIAL" >> "$LOGDIR/table.txt"
    fi
fi

# --- the repository, twice --------------------------------------------------
if [ -n "$TAG" ]; then
    run "integrity ($TAG)" python3 tools/integrity_check.py --expect-tag "$TAG"
else
    run "integrity" python3 tools/integrity_check.py
fi

# A build from an empty directory: the tree has to be enough on its own, with
# no object of a previous build and no generated file of one.
run "release from empty" sh -c 'rm -rf build-release && cmake -B build-release -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-release >/dev/null \
    && ctest --test-dir build-release --output-on-failure'

# --- the table --------------------------------------------------------------
echo
echo "the gate of the release ($TOTAL gate(s), $FAILED failed)"
echo
cat "$LOGDIR/table.txt"

exit "$FAILED"
