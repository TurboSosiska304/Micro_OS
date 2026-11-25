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
#   --no-wait  the host is not waited for: the gate starts on a busy machine
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
        --no-wait) NOWAIT=1; shift ;;
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

# The host has to be quiet: the smokes of QEMU lose a console, a frame or a
# boot under the load of the builds of another lane, and a gate of the release
# that runs beside one reports flakes of the host as if they were defects
# (x86-smoke answered 154 blocks of 310 in the gate of 1.8.0 with three lanes
# building on the same twelve cores, and 405 checks alone a minute later; in the
# gate of 1.9.0-rc1 the same smoke answered 8 blocks of 312 and then all 312 on
# a quiet machine). The gate waits for the load of the minute to fall under two,
# says how long it waited, and `--no-wait` takes the machine as it is.
CORES="$(nproc)"
# Half of the cores was too generous: a machine of twelve cores with a lane
# building on five of them has a load of five and a console of QEMU that loses
# a prompt - the smoke of the machine answered 8 blocks of 312 in the gate of
# 1.9.0-rc1 and 312 of 312 on the same tree an hour later, with the lanes
# stopped. The gate wants two.
LIMIT=2
if [ "$CORES" -lt 4 ]; then
    LIMIT=1
fi
WAITED=0
if [ "${NOWAIT:-0}" -eq 0 ]; then
    while :; do
        LOAD1="$(cut -d' ' -f1 /proc/loadavg)"
        BUSY="$(awk -v l="$LOAD1" -v m="$LIMIT" 'BEGIN { print (l > m) ? 1 : 0 }')"
        if [ "$BUSY" -eq 0 ]; then
            break
        fi
        if [ "$WAITED" -ge "${WAIT_MAX:-1800}" ]; then
            echo "release-gate: the load is $LOAD1 on $CORES core(s) after ${WAITED}s of waiting" \
                 "- the numbers below are of a busy host" >&2
            break
        fi
        if [ "$WAITED" -eq 0 ]; then
            echo "release-gate: the load is $LOAD1, waiting for $LIMIT (a quiet host)"
        fi
        sleep 15
        WAITED=$((WAITED + 15))
    done
fi
printf '| the host | %s core(s), load %s, waited %s s |\n' "$CORES" \
    "$(cut -d' ' -f1 /proc/loadavg)" "$WAITED" >> "$LOGDIR/table.txt"

# The smokes of QEMU are sensitive to the load of the host - the tree says so in
# several places, and a gate that runs one after another on a machine that also
# builds is the heaviest load they meet: an image that loses its console under
# that load has not lost it on a quiet machine. A gate that fails is run a
# second time, on the record: the line of the table says "second run", and a
# real defect fails twice.
# The load of a smoke is not only the load of the meter at the start of the
# gate: the build of the image the smoke boots is a build of three jobs, and a
# smoke that starts a minute after one is a smoke under load (x86-disk-smoke
# failed twice in the gate of 1.9.0-rc1 and passed alone a minute later, with
# the same tree). Every smoke waits for the host to be quiet, again.
wait_quiet() {
    waited=0
    while :; do
        load1="$(cut -d' ' -f1 /proc/loadavg)"
        busy="$(awk -v l="$load1" -v m="$LIMIT" 'BEGIN { print (l > m) ? 1 : 0 }')"
        if [ "$busy" -eq 0 ]; then
            return 0
        fi
        if [ "$waited" -ge 300 ]; then
            echo " (the load was $load1 after ${waited}s)"
            return 0
        fi
        sleep 10
        waited=$((waited + 10))
    done
}

run() {
    name="$1"
    shift
    TOTAL=$((TOTAL + 1))
    log="$LOGDIR/$(echo "$name" | tr ' /' '__').log"
    case "$name" in
        *smoke*) wait_quiet ;;
    esac

    if "$@" > "$log" 2>&1; then
        detail="$(sed -n 's/.*OK: \([0-9]* check(s)\) passed.*/\1/p' "$log" | tail -1)"
        printf '%-26s PASS  %s\n' "$name" "${detail:-}"
        printf '| `%s` | PASS %s |\n' "$name" "${detail:-(the build answered)}" >> "$LOGDIR/table.txt"
        return 0
    fi

    printf '%-26s second run\n' "$name"
    if "$@" > "$log.second" 2>&1; then
        detail="$(sed -n 's/.*OK: \([0-9]* check(s)\) passed.*/\1/p' "$log.second" | tail -1)"
        printf '%-26s PASS  %s (the second run; the first is in %s)\n' "$name" \
            "${detail:-}" "$log"
        printf '| `%s` | PASS %s on the second run (the first is in `%s`) |\n' \
            "$name" "${detail:-(the build answered)}" "$log" >> "$LOGDIR/table.txt"
        return 0
    fi

    FAILED=$((FAILED + 1))
    printf '%-26s FAIL  %s\n' "$name" "$log.second"
    printf '| `%s` | FAIL (%s, twice) |\n' "$name" "$log.second" >> "$LOGDIR/table.txt"
    return 1
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
run "mmu-smoke" m mmu-smoke
run "fork-smoke" m fork-smoke
run "x86-vga-smoke" m x86-vga-smoke
run "x86-disk-smoke" m x86-disk-smoke
run "x86-root-smoke" m x86-root-smoke
run "x86-volume-smoke" m x86-volume-smoke
run "x86-mfs-smoke" m x86-mfs-smoke
run "x86-cut-smoke" m x86-cut-smoke
run "x86-smp-smoke" m x86-smp-smoke
run "x86-net-smoke" m x86-net-smoke
run "usb-smoke" m usb-smoke
run "ports-smoke" m ports-smoke
run "ahci-smoke" m ahci-smoke
run "nvme-smoke" m nvme-smoke
run "power-smoke" m power-smoke
run "uefi-smoke" m uefi-smoke
run "uefi-stick-smoke" m uefi-stick-smoke
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
