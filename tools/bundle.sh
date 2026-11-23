#!/usr/bin/env bash
#
# Micro OS — collect the built artifacts into the distribution folder.
#
# The bundle is the folder dist/: the firmware that is flashed onto the board,
# the archive of the x86-64 machine that is run, the sums that prove they are
# the ones that were built, and the static files of the two distributions.
#
# Usage:
#   tools/bundle.sh [--x86-only | --board-only] [BUILD_DIR] [OUT_DIR]
#
# Without a flag both machines are packed: the firmware of the board and the
# x86-64 archive. --board-only packs the firmware alone; it needs the finished
# ESP-IDF build, esptool and the partition table. --x86-only packs the archive
# of the x86-64 machine alone; it needs neither the firmware build nor
# esptool, because that machine is run, not flashed.
#
# BUILD_DIR is the directory of the ESP-IDF build (default: build); with
# --x86-only it is the build of the port (default: build-x86). X86_BUILD names
# the build of the port when both machines are packed (default: build-x86, or
# build-x86-all when the complete machine is there). OUT_DIR is the folder of
# the bundle (default: dist).
#
# The script needs the finished builds, but not the ESP-IDF itself.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "usage: tools/bundle.sh [--x86-only | --board-only] [BUILD_DIR] [OUT_DIR]"
}

MODE=both
while [ $# -gt 0 ]; do
    case "$1" in
        --x86-only) MODE=x86; shift ;;
        --board-only) MODE=board; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) echo "bundle: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) break ;;
    esac
done

# The two machines are built by different projects: the board in the ESP-IDF
# tree, the port in its own. The first directory named on the command line is
# the build of the mode that is packed.
if [ "$MODE" = x86 ]; then
    BUILD_DIR="${1:-$ROOT/build-x86}"
else
    BUILD_DIR="${1:-$ROOT/build}"
fi
OUT_DIR="${2:-$ROOT/dist}"
STATIC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bundle"

# The build of the port. With --x86-only it is the directory named above; a
# full bundle prefers the complete machine of `make x86-all`, and the plain
# kernel of the port is the last resort.
X86_BUILD="${X86_BUILD:-}"
if [ -z "$X86_BUILD" ] && [ "$MODE" = x86 ]; then
    X86_BUILD="$BUILD_DIR"
fi
X86_BUILD="${X86_BUILD:-$ROOT/build-x86}"

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
X86_NAME="MicroOS-x86_64-${VERSION}"

# The board: the firmware and the files that flash it. Everything here needs
# the ESP-IDF build, esptool and the partition table of the board.
board_bundle() {
    # The files of the build and the names they get in the bundle.
    # "source:target" — the source is relative to the build directory.
    BINARIES=(
        "micro_os.bin:micro_os.bin"
        "bootloader/bootloader.bin:bootloader.bin"
        "partition_table/partition-table.bin:partition-table.bin"
        "esp-idf/port_esp32/progs.bin:progs.bin"
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

    # The update tool is the one of the tree, not a copy in the folder of the
    # static files: it reads the bundle around itself and names no build.
    cp -f "$ROOT/tools/ota_usb.py" "$OUT_DIR/ota_usb.py"
    echo "    tools/ota_usb.py -> ota_usb.py"

    # The license of the one third-party component of the x86-64 image travels
    # with the bundle, as the file of /etc/licenses on the disk does.
    cp -f "$ROOT/third_party/pcre2/LICENSE" "$OUT_DIR/LICENSE.pcre2"
    echo "    third_party/pcre2/LICENSE -> LICENSE.pcre2"

    chmod +x "$OUT_DIR/flash.sh" "$OUT_DIR/ota_usb.py"

    # One image for the board: the boot loader, the table of the partitions and
    # the application, at the offsets the flash expects. It covers 0x0..the end
    # of the application; the NVS, otadata, the programs and MicroFS live further
    # and are not touched by a flash of this file. The layout is the one of
    # ADR 0022: the first application slot (ota_0) carries the installed image.
    APP_OFFSET="$(awk -F',' '/^ota_0[ \t]*,/ {gsub(/[ \t]/,"",$4); print $4}' partitions.csv)"
    APP_OFFSET="${APP_OFFSET:-0x10000}"
    APP_SIZE_PARTITION="$(awk -F',' '/^ota_0[ \t]*,/ {gsub(/[ \t]/,"",$5); print $5}' partitions.csv)"
    APP_SIZE_PARTITION="${APP_SIZE_PARTITION:-0x120000}"

    # The application and the 4 KiB of the signature trailer share the slot
    # (ADR 0020, ADR 0022). The check is here as well as in the build: a bundle
    # made from an older build directory must not put the trailer over the app.
    APP_SIZE="$(stat -c %s "$OUT_DIR/micro_os.bin")"
    APP_LIMIT=$((APP_SIZE_PARTITION - 0x1000))
    if [ "$APP_SIZE" -gt "$APP_LIMIT" ]; then
        echo "the image is $APP_SIZE bytes and the partition leaves $APP_LIMIT for it and its trailer" >&2
        exit 1
    fi

    # The programs of the board are their own partition (ADR 0018 §2, ADR 0022):
    # a bundle carries the image the build made, and it has to be exactly the
    # partition of the table, or the board reads a directory from the wrong place.
    PROGS_SIZE="$(awk -F',' '/^progs[ \t]*,/ {gsub(/[ \t]/,"",$5); print $5}' partitions.csv)"
    PROGS_SIZE="${PROGS_SIZE:-0x21000}"
    PROGS_BYTES="$(stat -c %s "$OUT_DIR/progs.bin")"
    if [ "$PROGS_BYTES" -ne $((PROGS_SIZE)) ]; then
        echo "progs.bin is $PROGS_BYTES bytes and the partition is $PROGS_SIZE" >&2
        exit 1
    fi

    echo "==> the single image $MERGED"
    python3 -m esptool --chip esp32 merge_bin -o "$OUT_DIR/$MERGED" \
        --flash_mode dio --flash_freq 40m --flash_size 8MB \
        0x1000 "$OUT_DIR/bootloader.bin" \
        0x8000 "$OUT_DIR/partition-table.bin" \
        "$APP_OFFSET" "$OUT_DIR/micro_os.bin"
    echo "    $MERGED ($(stat -c %s "$OUT_DIR/$MERGED") bytes)"

    # The signature of the bundle (ADR 0020). The key of a release is named by
    # MICROOS_SIGN_KEY and lives outside this tree; a development build signs with
    # the key of its own firmware (build/signing.key, the public half is compiled
    # into the image), and the x86-64 key is the last resort. The trailer of the
    # app is written beside it and flash.sh puts it at 0x12F000; a copy of the
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

        # The board reads the trailer at the end of the running application slot,
        # not after the app: the signature is written beside the image and
        # flash.sh puts it at 0x12F000 of ota_0 (ADR 0020, ADR 0022). The
        # combined file is a step of the tool, not a file of the bundle.
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
    SUMS=("$MERGED" micro_os.bin progs.bin bootloader.bin partition-table.bin)
    if [ -f "$OUT_DIR/micro_os.bin.sig" ]; then
        SUMS=(micro_os.bin.sig "${SUMS[@]}")
    fi
    (cd "$OUT_DIR" && sha256sum "${SUMS[@]}" > checksums.txt)
    if [ -f "$OUT_DIR/$MERGED.sig" ]; then
        (cd "$OUT_DIR" && sha256sum "$MERGED.sig" microos-sign.pub >> checksums.txt)
    fi

    echo "==> the sums"
    cat "$OUT_DIR/checksums.txt"
}

# The other machine: the loader and the kernel of the x86-64 port, when the
# build of that port is there. It is a bundle of its own, because a machine
# without a board is run, not flashed.
#
# The complete machine (`make x86-all`) is the artifact a person gets: the
# kernel with the network, the disk with /bin and /etc, and a run.sh that
# starts them together. A tree that has not built it falls back to the
# kernel alone, with the memory volume of the port.
x86_bundle() {
    X86_SRC="$X86_BUILD"
    if [ "$MODE" != x86 ] && [ -f "$ROOT/build-x86-all/loader32.elf" ] && \
       [ -f "$ROOT/build-x86-all/x86-all.img" ]; then
        X86_SRC="$ROOT/build-x86-all"
    fi
    X86_DISK=""
    if [ -f "$X86_SRC/x86-all.img" ]; then
        X86_DISK="$X86_SRC/x86-all.img"
    fi

    if [ ! -f "$X86_SRC/loader32.elf" ] || [ ! -f "$X86_SRC/microos64.bin" ]; then
        if [ "$MODE" = x86 ]; then
            echo "the x86-64 build is incomplete: $X86_SRC has no loader32.elf" \
                "or microos64.bin" >&2
            echo "build it first: make x86-all" >&2
            exit 1
        fi
        echo "==> the x86-64 port is not built ($X86_SRC), skipped" >&2
        return 0
    fi

    mkdir -p "$OUT_DIR"
    echo "==> the bundle of the x86-64 port: $X86_NAME.tar.gz"

    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT

    cp -f "$X86_SRC/loader32.elf" "$STAGE/loader32.elf"
    cp -f "$X86_SRC/microos64.bin" "$STAGE/microos64.bin"
    if [ -n "$X86_DISK" ]; then
        cp -f "$X86_DISK" "$STAGE/x86-all.img"
    fi
    cp -f "$ROOT/third_party/spleen/LICENSE" "$STAGE/LICENSE.spleen"
    # zlib is linked into /bin/zcat of the disk image, and its license travels
    # with the distribution even when the image itself is built separately.
    cp -f "$ROOT/third_party/zlib/LICENSE" "$STAGE/LICENSE.zlib"
    # PCRE2 is linked into /bin/pcre2grep, and its license travels the same
    # way: the notice beside the program and the text in the archive.
    cp -f "$ROOT/third_party/pcre2/LICENSE" "$STAGE/LICENSE.pcre2"
    # Lua is /bin/lua of the same image, under the MIT license of its release.
    cp -f "$ROOT/third_party/lua/LICENSE" "$STAGE/LICENSE.lua"
    # The tools of sbase are the commands of /bin of the same image (cat,
    # grep, ls, ...), under their MIT license; the notice travels with the
    # archive for the same reason.
    cp -f "$ROOT/third_party/sbase/LICENSE" "$STAGE/LICENSE.sbase"
    # bc and dc are two names of the same engine and one BSD-2-Clause release.
    cp -f "$ROOT/third_party/bc/LICENSE" "$STAGE/LICENSE.bc"
    cp -f "$ROOT/third_party/mbedtls/LICENSE" "$STAGE/LICENSE.mbedtls"
    if [ -f "$X86_SRC/microos64.sig" ] && [ -f "$X86_SRC/signing.pub" ]; then
        cp -f "$X86_SRC/microos64.sig" "$STAGE/microos64.sig"
        cp -f "$X86_SRC/signing.pub" "$STAGE/microos-sign.pub"
    fi

    cat > "$STAGE/run.sh" <<'RUN'
#!/usr/bin/env bash
# Micro OS on an x86-64 machine: the loader and the kernel, with the disk of
# the complete machine and its network when they are in this directory.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARGS=(-kernel "$HERE/loader32.elf" -initrd "$HERE/microos64.bin" -m 256 -no-reboot)

if [ -f "$HERE/x86-all.img" ]; then
    ARGS+=(-drive "file=$HERE/x86-all.img,format=raw,if=ide,index=0,cache=writeback"
           -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080
           -device virtio-net-pci,netdev=n0,disable-modern=on)
fi

if [ "${X86_SERIAL:-0}" = 1 ]; then
    exec qemu-system-x86_64 "${ARGS[@]}" -nographic
fi

exec qemu-system-x86_64 "${ARGS[@]}" -display "${X86_DISPLAY:-gtk}" -serial null
RUN
    chmod +x "$STAGE/run.sh"

    cat > "$STAGE/README.md" <<'DOC'
# Micro OS for x86-64

The kernel of Micro OS, built for a bare x86-64 machine: the loader
(`loader32.elf`) copies the kernel (`microos64.bin`) to its window and turns
the long mode on. There is no operating system under it.

Run it with QEMU:

    ./run.sh

The console is COM1 and the VGA screen. The machine has the shell, the
interrupts, the scheduler, the disk with MicroFS, the programs of /bin and
the network of the port.

The kernel carries Spleen 8x16 (BSD-2-Clause) for the framebuffer console and,
in its network image, Mbed TLS 3.6.2 (Apache-2.0, the half of its dual grant
this tree takes) for TLS. `LICENSE.spleen` and `LICENSE.mbedtls` are their
licenses; `third_party/spleen` and `third_party/mbedtls` of the sources have
the notes and the versions.

`/bin/zcat` of the disk image is linked against zlib 1.3.1 (zlib license);
`LICENSE.zlib` is its license and `third_party/zlib` of the sources has the
note, the version and the port layer.

`/bin/lua` of the disk image is Lua 5.4.7 (MIT license); `LICENSE.lua` is its
license and `third_party/lua` of the sources has the note, the version, the
port layer and the one setting of the port (`port/luaconf_port.h`). The
interpreter runs out of the volume: `make x86-disk-smoke` reads its script in
the first boot and after the reset.

`/bin/cat`, `/bin/grep`, `/bin/ls` and the rest of the ten commands are
the tools of sbase (MIT); `LICENSE.sbase` is their license and
`third_party/sbase` of the sources has the subset and the port layer.

`/bin/bc` and `/bin/dc` of the disk image are bc 7.2.0 (BSD-2-Clause), one
engine with two names; `LICENSE.bc` is its license and `third_party/bc` of the
sources has the note, the version, the port layer and the settings of the
port (`port/bc_port.h`).

The kernel carries a signature trailer (ADR 0020), and the loader verifies it
before it copies a byte: an image with a changed byte, without a trailer or
signed by another key stops with `the image is refused` and never reaches the
kernel. `microos64.sig` is a copy of the trailer and `microos-sign.pub` is the
public key of the build; the tool in `tools/` of the sources verifies them:

    microos_sign verify --pub microos-sign.pub microos64.bin

The same kernel is on the board and in the tests: Micro OS is one portable
kernel with a port per machine.

The machine carries three third-party components: `/bin/pcre2grep` links
PCRE2 10.42, `/bin/lua` links the engine of Lua 5.4.7, the ten commands of
sbase are the programs of the collection, and `/bin/bc` and `/bin/dc` are the
two names of one engine. `LICENSE.pcre2`, `LICENSE.lua`, `LICENSE.sbase` and
`LICENSE.bc` beside this file are the licenses of that code, and the texts
are files of the disk image under `/etc/licenses/`; the MIT of Micro OS does
not cover a byte of them.
DOC

    STAGE_FILES=(loader32.elf microos64.bin run.sh README.md LICENSE.spleen LICENSE.zlib
                 LICENSE.pcre2 LICENSE.lua LICENSE.sbase LICENSE.bc)
    if [ -f "$STAGE/x86-all.img" ]; then
        STAGE_FILES+=(x86-all.img)
    fi
    if [ -f "$STAGE/microos64.sig" ]; then
        STAGE_FILES+=(microos64.sig microos-sign.pub)
    fi

    (cd "$STAGE" && tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
        --owner=0 --group=0 --numeric-owner --format=gnu -cf - \
        "${STAGE_FILES[@]}" | gzip -n > "$OUT_DIR/$X86_NAME.tar.gz")
    echo "    $X86_NAME.tar.gz ($(stat -c %s "$OUT_DIR/$X86_NAME.tar.gz") bytes)"
    # The sums of the folder: a full bundle has just written the board's, and
    # an archive packed alone owns the file. The line of this archive is
    # replaced rather than added twice, so a second run of the same mode
    # leaves one sum of one file.
    SUMS_FILE="$OUT_DIR/checksums.txt"
    if [ -f "$SUMS_FILE" ]; then
        grep -v "  $X86_NAME.tar.gz\$" "$SUMS_FILE" > "$SUMS_FILE.tmp" || true
        mv "$SUMS_FILE.tmp" "$SUMS_FILE"
    fi
    (cd "$OUT_DIR" && sha256sum "$X86_NAME.tar.gz" >> checksums.txt)
    echo "==> the sums"
    cat "$OUT_DIR/checksums.txt"
}

case "$MODE" in
    both)
        board_bundle
        x86_bundle
        echo "==> done. Flash the board with: $OUT_DIR/flash.sh"
        echo "    the machine of x86-64: $OUT_DIR/$X86_NAME.tar.gz (run ./run.sh after unpacking)"
        ;;
    board)
        board_bundle
        echo "==> done. Flash the board with: $OUT_DIR/flash.sh"
        ;;
    x86)
        x86_bundle
        echo "==> done. Unpack it and run: tar -xzf $X86_NAME.tar.gz && ./run.sh"
        ;;
esac
