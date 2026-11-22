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

# The same inputs, the same bytes: the archive of the x86-64 port gets a
# fixed mtime, the numeric owner 0 and no name of the machine. The moment is
# the convention of a reproducible build, and the Makefile exports it.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
export SOURCE_DATE_EPOCH TZ=UTC LC_ALL=C
umask 022

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
FACTORY_SIZE="$(awk -F',' '/^factory/ {gsub(/[ \t]/,"",$5); print $5}' partitions.csv)"
FACTORY_SIZE="${FACTORY_SIZE:-0x200000}"

# The application and the 4 KiB of the signature trailer share the factory
# partition (ADR 0020). The check is here as well as in the build: a bundle
# made from an older build directory must not put the trailer over the app.
APP_SIZE="$(stat -c %s "$OUT_DIR/micro_os.bin")"
APP_LIMIT=$((FACTORY_SIZE - 0x1000))
if [ "$APP_SIZE" -gt "$APP_LIMIT" ]; then
    echo "the image is $APP_SIZE bytes and the partition leaves $APP_LIMIT for it and its trailer" >&2
    exit 1
fi

echo "==> the single image $MERGED"
python3 -m esptool --chip esp32 merge_bin -o "$OUT_DIR/$MERGED" \
    --flash_mode dio --flash_freq 40m --flash_size 8MB \
    0x1000 "$OUT_DIR/bootloader.bin" \
    0x8000 "$OUT_DIR/partition-table.bin" \
    "$FACTORY_OFFSET" "$OUT_DIR/micro_os.bin"
echo "    $MERGED ($(stat -c %s "$OUT_DIR/$MERGED") bytes)"

# The signature of the bundle (ADR 0020). The key of a release is named by
# MICROOS_SIGN_KEY and lives outside this tree; a development build signs with
# the key of its own firmware (build/signing.key, the public half is compiled
# into the image), and the x86-64 key is the last resort. The trailer of the
# app is written beside it and flash.sh puts it at 0x20F000; a copy of the
# merged image's trailer is its .sig, so a person can verify the bytes with
# `microos_sign verify`.
X86_BUILD="${X86_BUILD:-$ROOT/build-x86}"
SIGNER="${MICROOS_SIGNER:-}"
if [ -z "$SIGNER" ]; then
    for candidate in "$ROOT/build-host/tools/microos_sign" "$X86_BUILD/microos_sign" \
                     "$BUILD_DIR/microos_sign_host"; do
        if [ -x "$candidate" ]; then
            SIGNER="$candidate"
            break
        fi
    done
fi

# The key of the firmware is the one its public half was compiled from: the
# release key (MICROOS_SIGN_KEY) or the development key of the ESP-IDF build.
# The key of the x86-64 build is the last resort, for a bundle of a tree that
# has no firmware build.
SIGN_KEY="${MICROOS_SIGN_KEY:-}"
if [ -z "$SIGN_KEY" ] && [ -f "$BUILD_DIR/signing.key" ]; then
    SIGN_KEY="$BUILD_DIR/signing.key"
fi
if [ -z "$SIGN_KEY" ] && [ -f "$X86_BUILD/signing.key" ]; then
    SIGN_KEY="$X86_BUILD/signing.key"
fi

if [ -n "$SIGNER" ] && [ -n "$SIGN_KEY" ]; then
    echo "==> the signature of the application: micro_os.bin"
    "$SIGNER" pubkey --key "$SIGN_KEY" --pub "$OUT_DIR/microos-sign.pub"

    # The board reads the trailer at the end of the factory partition, not
    # after the app: the signature is written beside the image and flash.sh
    # puts it at 0x20F000. The combined file is a step of the tool, not a file
    # of the bundle.
    "$SIGNER" sign --key "$SIGN_KEY" --in "$OUT_DIR/micro_os.bin" \
        --out "$OUT_DIR/micro_os.signed.bin" --sig "$OUT_DIR/micro_os.bin.sig"
    rm -f "$OUT_DIR/micro_os.signed.bin"
    "$SIGNER" verify --pub "$OUT_DIR/microos-sign.pub" \
        --trailer "$OUT_DIR/micro_os.bin.sig" "$OUT_DIR/micro_os.bin"

    echo "==> the signature of the distribution: $MERGED"
    "$SIGNER" sign --key "$SIGN_KEY" --in "$OUT_DIR/$MERGED" --out "$OUT_DIR/$MERGED" \
        --sig "$OUT_DIR/$MERGED.sig"
    "$SIGNER" verify --pub "$OUT_DIR/microos-sign.pub" "$OUT_DIR/$MERGED"
else
    echo "==> no signing key (MICROOS_SIGN_KEY): the bundle is not signed" >&2
fi

# The sums of the binaries: they are the proof that the bundle is intact.
SUMS=("$MERGED" micro_os.bin bootloader.bin partition-table.bin)
if [ -f "$OUT_DIR/micro_os.bin.sig" ]; then
    SUMS=(micro_os.bin.sig "${SUMS[@]}")
fi
(cd "$OUT_DIR" && sha256sum "${SUMS[@]}" > checksums.txt)
if [ -f "$OUT_DIR/$MERGED.sig" ]; then
    (cd "$OUT_DIR" && sha256sum "$MERGED.sig" microos-sign.pub >> checksums.txt)
fi

echo "==> the sums"
cat "$OUT_DIR/checksums.txt"

# The other machine: the loader and the kernel of the x86-64 port, when the
# build of that port is there. It is a bundle of its own, because a machine
# without a board is run, not flashed.
X86_NAME="MicroOS-x86_64-${VERSION}"

if [ -f "$X86_BUILD/loader32.elf" ] && [ -f "$X86_BUILD/microos64.bin" ]; then
    echo "==> the bundle of the x86-64 port: $X86_NAME.tar.gz"

    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT

    cp -f "$X86_BUILD/loader32.elf" "$STAGE/loader32.elf"
    cp -f "$X86_BUILD/microos64.bin" "$STAGE/microos64.bin"
    if [ -f "$X86_BUILD/microos64.sig" ] && [ -f "$X86_BUILD/signing.pub" ]; then
        cp -f "$X86_BUILD/microos64.sig" "$STAGE/microos64.sig"
        cp -f "$X86_BUILD/signing.pub" "$STAGE/microos-sign.pub"
    fi

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

The kernel carries a signature trailer (ADR 0020), and the loader verifies it
before it copies a byte: an image with a changed byte, without a trailer or
signed by another key stops with `the image is refused` and never reaches the
kernel. `microos64.sig` is a copy of the trailer and `microos-sign.pub` is the
public key of the build; the tool in `tools/` of the sources verifies them:

    microos_sign verify --pub microos-sign.pub microos64.bin

The same kernel is on the board and in the tests: Micro OS is one portable
kernel with a port per machine.
DOC

    STAGE_FILES=(loader32.elf microos64.bin run.sh README.md)
    if [ -f "$STAGE/microos64.sig" ]; then
        STAGE_FILES+=(microos64.sig microos-sign.pub)
    fi

    (cd "$STAGE" && tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
        --owner=0 --group=0 --numeric-owner --format=gnu -cf - \
        "${STAGE_FILES[@]}" | gzip -n > "$OUT_DIR/$X86_NAME.tar.gz")
    echo "    $X86_NAME.tar.gz ($(stat -c %s "$OUT_DIR/$X86_NAME.tar.gz") bytes)"
    (cd "$OUT_DIR" && sha256sum "$X86_NAME.tar.gz" >> checksums.txt)
else
    echo "==> the x86-64 port is not built ($X86_BUILD), skipped"
fi

echo "==> done. Flash it with: $OUT_DIR/flash.sh"
