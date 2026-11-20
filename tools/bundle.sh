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

# The stage of the x86-64 bundle is another working directory for a moment,
# so the folder of the bundle has to be absolute before the script gets there.
case "$OUT_DIR" in
    /*) ;;
    *) OUT_DIR="$ROOT/$OUT_DIR" ;;
esac

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

# The version of the kernel, as the sources say it: the name of the images.
VERSION="$(python3 - <<'PY'
import pathlib
import re

text = pathlib.Path("components/kernel/include/kernel/config.h").read_text()


def number(name):
    match = re.search(rf"#define {name} (\d+)", text)
    return match.group(1)


suffix = re.search(r'#define KERNEL_VERSION_SUFFIX "(.*)"', text).group(1)
print(f"{number('KERNEL_VERSION_MAJOR')}.{number('KERNEL_VERSION_MINOR')}."
      f"{number('KERNEL_VERSION_PATCH')}{suffix}")
PY
)"
MERGED="MicroOS-${VERSION}.bin"

# One image for the board: the boot loader, the table of the partitions and
# the application, at the offsets the flash expects. It covers 0x0..the end
# of the application; the NVS and MicroFS live further and are not touched
# by a flash of this file.
FACTORY_OFFSET="$(awk -F',' '/^factory/ {gsub(/[ \t]/,"",$4); print $4}' partitions.csv)"
FACTORY_OFFSET="${FACTORY_OFFSET:-0x10000}"

echo "==> the single image $MERGED"
python3 -m esptool --chip esp32 merge_bin -o "$OUT_DIR/$MERGED" \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x1000 "$OUT_DIR/bootloader.bin" \
    0x8000 "$OUT_DIR/partition-table.bin" \
    "$FACTORY_OFFSET" "$OUT_DIR/micro_os.bin"
echo "    $MERGED ($(stat -c %s "$OUT_DIR/$MERGED") bytes)"

# The sums of the binaries: they are the proof that the bundle is intact.
(cd "$OUT_DIR" && sha256sum "$MERGED" micro_os.bin bootloader.bin partition-table.bin > checksums.txt)

echo "==> the sums"
cat "$OUT_DIR/checksums.txt"

# The other machine: the loader and the kernel of the x86-64 port, when the
# build of that port is there. It is a bundle of its own, because a machine
# without a board is run, not flashed.
X86_BUILD="${X86_BUILD:-$ROOT/build-x86}"
X86_NAME="MicroOS-x86_64-${VERSION}"

if [ -f "$X86_BUILD/loader32.elf" ] && [ -f "$X86_BUILD/microos64.bin" ]; then
    echo "==> the bundle of the x86-64 port: $X86_NAME.tar.gz"

    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT

    cp -f "$X86_BUILD/loader32.elf" "$STAGE/loader32.elf"
    cp -f "$X86_BUILD/microos64.bin" "$STAGE/microos64.bin"

    cat > "$STAGE/run.sh" <<'RUN'
#!/usr/bin/env bash
# Micro OS on an x86-64 machine: the loader and the kernel, no board.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec qemu-system-x86_64 -kernel "$HERE/loader32.elf" -initrd "$HERE/microos64.bin" \
    -m 256 -nographic -no-reboot
RUN
    chmod +x "$STAGE/run.sh"

    cat > "$STAGE/README.md" <<'DOC'
# Micro OS for x86-64

The kernel of Micro OS, built for a bare x86-64 machine: the loader
(`loader32.elf`) copies the kernel (`microos64.bin`) to its window and turns
the long mode on. There is no operating system under it.

Run it with QEMU:

    ./run.sh

The console is COM1. The machine has the shell, the interrupts, the
scheduler; the filesystem and the network of this port are still ahead.

The same kernel is on the board and in the tests: Micro OS is one portable
kernel with a port per machine.
DOC

    (cd "$STAGE" && tar -czf "$OUT_DIR/$X86_NAME.tar.gz" loader32.elf microos64.bin run.sh README.md)
    echo "    $X86_NAME.tar.gz ($(stat -c %s "$OUT_DIR/$X86_NAME.tar.gz") bytes)"
else
    echo "==> the x86-64 port is not built ($X86_BUILD), skipped"
fi

echo "==> done. Flash it with: $OUT_DIR/flash.sh"
