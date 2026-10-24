#!/usr/bin/env bash
#
# Micro OS — collect the built firmware into the flash bundle.
#
# The bundle is the folder dist/: the binaries that are flashed onto the
# board, the sums that prove they are the ones that were built, and the two
# static files of the folder (the flash script and its description).
#
# Usage:
#   tools/bundle.sh [BUILD_DIR] [OUT_DIR]
#
# BUILD_DIR is the directory of the ESP-IDF build (default: build),
# OUT_DIR is the folder of the bundle (default: dist).
#
# The script needs the finished ESP-IDF build, but not the ESP-IDF itself.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
OUT_DIR="${2:-$ROOT/dist}"
STATIC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bundle"

cd "$ROOT"

# The files of the build and the names they get in the bundle.
# "source:target" — the source is relative to the build directory.
BINARIES=(
    "micro_os.bin:micro_os.bin"
    "bootloader/bootloader.bin:bootloader.bin"
    "partition_table/partition-table.bin:partition-table.bin"
)

echo "==> the bundle goes to $OUT_DIR"

for entry in "${BINARIES[@]}"; do
    source_file="${entry%%:*}"
    target_file="${entry##*:}"
    full_source="$BUILD_DIR/$source_file"

    if [ ! -f "$full_source" ]; then
        echo "the build is incomplete: $full_source is missing" >&2
        echo "build the firmware first: make idf-build" >&2
        exit 1
    fi

    mkdir -p "$OUT_DIR"
    cp -f "$full_source" "$OUT_DIR/$target_file"
    echo "    $source_file -> $target_file ($(stat -c %s "$OUT_DIR/$target_file") bytes)"
done

# The files of the bundle that are not built: the flash script and the
# description of the folder. They live in the repository and travel with
# every bundle, so that a bundle is always ready to be flashed.
for name in flash.sh README.md; do
    if [ ! -f "$STATIC/$name" ]; then
        echo "the static file of the bundle is missing: $STATIC/$name" >&2
        exit 1
    fi
    cp -f "$STATIC/$name" "$OUT_DIR/$name"
    echo "    tools/bundle/$name -> $name"
done
chmod +x "$OUT_DIR/flash.sh"

# The sums of the binaries: they are the proof that the bundle is intact.
(cd "$OUT_DIR" && sha256sum micro_os.bin bootloader.bin partition-table.bin > checksums.txt)

echo "==> the sums"
cat "$OUT_DIR/checksums.txt"

echo "==> done. Flash it with: $OUT_DIR/flash.sh"
