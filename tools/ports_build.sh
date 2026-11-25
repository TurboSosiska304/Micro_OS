#!/bin/sh
#
# Micro OS — build the programs of `make ports-smoke` with the SDK alone.
#
#     tools/ports_build.sh [--build build-ports] [--heap <bytes>] [--verbose]
#
# The two steps are the two halves of the promise of the lane: `make sdk`
# produces the library, the crt, the linker script and the toolchain file of
# the release, and the CMake project of `tests/ports` is a project of a
# person with that SDK - it names the sources of the release, the helpers of
# the collection and nothing of this tree. A program that needs a header or a
# function the library does not have stops here, and the ticket of the lane
# is that no line of `tests/ports/sbase/` is edited to make it pass.
#
# The build directory is not a part of the tree that a release carries:
# `build-ports/` is in `.gitignore` like every other `build*/`.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=build-ports
heap=262144
verbose=0

usage()
{
    echo "usage: $0 [--build <dir>] [--heap <bytes>] [--verbose]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build)
            [ "$#" -ge 2 ] || usage
            build=$2
            shift 2
            ;;
        --heap)
            [ "$#" -ge 2 ] || usage
            heap=$2
            shift 2
            ;;
        --verbose)
            verbose=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "$0: unknown argument: $1" >&2
            usage
            ;;
    esac
done

case "$heap" in
    ""|*[!0-9]*)
        echo "$0: --heap wants a number of bytes, not '$heap'" >&2
        exit 2
        ;;
esac

cd "$root"

# The SDK of the release. The directory holds one version and the build
# replaces it: an SDK of another version beside this one would be a library
# of a release linked into the programs of another.
rm -rf "$build/sdk/microos-sdk"
sh tools/sdk/make_sdk.sh --destdir "$build/sdk" --heap "$heap"

sdk=$(find "$build/sdk/microos-sdk" -mindepth 1 -maxdepth 1 -type d -print)
[ -n "$sdk" ] || {
    echo "$0: the SDK of the build is not there" >&2
    exit 1
}
if [ "$(printf '%s\n' "$sdk" | wc -l)" -ne 1 ]; then
    echo "$0: $build/sdk holds more than one SDK" >&2
    exit 1
fi

# The toolchain file is named by its absolute path: the SDK finds its own root
# from the directory of that file, and a path relative to the build directory
# would name a second SDK that is not there.
sdk=$(cd "$sdk" && pwd)

# The base of the window is the port's, and it is read here from the one place
# that names it (PROGRAM_BASE of the port's CMake) and handed to the project:
# a cache of the build directory from an older SDK must not link a program of
# the release at a base the machine no longer maps (ADR 0057 moved it).
window=$(sed -n 's/^set(PROGRAM_BASE \(0x[0-9a-fA-F]*\))$/\1/p' \
    ports/port_x86_64/CMakeLists.txt | head -1)
if [ -z "$window" ]; then
    echo "$0: the port does not name its window (PROGRAM_BASE)" >&2
    exit 1
fi

# A warning of a source of the release is not a failure of this gate: the
# programs are somebody else's and answer the warnings of their release
# (MICROOS_WARNINGS=OFF); the warnings of the library of the tree are an
# error, because `make sdk` builds it with -Werror.
cmake -B "$build/programs" -S tests/ports -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/share/microos-toolchain.cmake" \
    -DMICROOS_PROGRAM_BASE="$window" \
    -DMICROOS_WARNINGS=OFF
if [ "$verbose" -eq 1 ]; then
    cmake --build "$build/programs" --verbose
else
    cmake --build "$build/programs"
fi

count=$(find "$build/programs" -maxdepth 1 -name '*.mox' | wc -l)
echo "ports: $count image(s) of /bin/ports in $build/programs"
