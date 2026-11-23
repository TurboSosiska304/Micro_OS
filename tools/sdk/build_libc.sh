#!/bin/sh
#
# Micro OS — the library of a program, built from the sources of the tree.
#
#     tools/sdk/build_libc.sh --src <dir> --include <dir> --out <lib.a> \
#         [--crt0 <crt0.o>] [--heap <bytes>] [--no-float] [--cc <command>] \
#         [--werror] [--objects <dir>]
#
# The library of a program is not the library of the kernel: every function of
# it is a thin layer over the one door a program has, `int $0x80` on x86-64
# (ADR 0006, ADR 0016), and a program carries its own copy of it. This script
# is the recipe of that copy, and it is the same recipe the SDK uses: the
# archive the tree ships and the archive a person rebuilds are built by the
# same words and the same flags.
#
# --src is `components/libc/user/src`, or the copy of it inside the SDK
# (`share/src/libc`); --include is a directory that carries `ulibc.h` and
# `kernel/ksys.h` (the SDK's `include/`, which is the headers of the library
# and of the ABI). `src/start.c` is not a part of the archive: it is the entry
# of a program, and it is written as a separate object with --crt0, because a
# linker takes one `_start` and the file of the program is not the library.
#
# The flags are the ones of a guest object in the tree (ports/port_x86_64/
# CMakeLists.txt): no libc of the host (-nostdinc and the two include paths
# below), no PIC and no PIE (the image is linked at an absolute window), no
# stack protector, no red zone (the port enters a program without one) and a
# section per function and per object, so that the linker of a program may
# drop what it does not call. -Os is the size of an image on the disk of the
# machine; the library is compiled once and every program pays for the part it
# names.
#
# The header of the compiler itself (stdarg.h, stddef.h, ...) is where the
# driver says it is: -nostdinc takes the directories of the system away, and
# `$cc -print-file-name=include` is the one directory that has to come back.
#
# --- the numbers of a build
#
# --heap is the arena of the allocator (`malloc`), the bss of the program and
# not a block of the kernel: 256 KiB is the default of src/malloc.c and of
# docs/porting-programs.md, and the format caps the bss of an image at a
# megabyte (K_EXEC_BSS_MAX). A program that needs more rebuilds the archive
# with another number; the arena belongs to the archive and not to the
# program, because src/malloc.c is one of its objects.
#
# A build with floating point (the default) compiles with the SSE and x87
# units allowed and defines ULIBC_WITH_FLOAT, so `%f`, `strtod`, `math.h` and
# `setjmp` of a double are there: the library is one archive for every
# program, and a program that does not print a double never reaches the code.
# --no-float builds the smaller library of a program that computes in
# integers: -mgeneral-regs-only, no ULIBC_WITH_FLOAT, and the three sources
# that need the unit (src/math.c, src/format_float.c and src/sscanf.c, whose
# strtod is a double) are left out. A program linked with it has no `%f` and
# no `sscanf`, which is the library of the tools of the tree (sbase).
#
# --werror is for the tree: the sources of this repository compile without a
# warning, and the SDK build of a release says so. A person rebuilding the
# archive with another compiler leaves it off.
set -eu

K_DEFAULT_HEAP=262144
K_NO_FLOAT_SKIP="format_float math sscanf"

usage()
{
    echo "usage: $0 --src <dir> --include <dir> --out <lib.a>" >&2
    echo "       [--crt0 <crt0.o>] [--heap <bytes>] [--no-float]" >&2
    echo "       [--cc <command>] [--werror] [--objects <dir>]" >&2
    exit 2
}

cc=${CC:-cc}
ar=${AR:-ar}
src=
include=
out=
crt0=
heap=$K_DEFAULT_HEAP
float=1
werror=0
objects=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cc)
            [ "$#" -ge 2 ] || usage
            cc=$2
            shift 2
            ;;
        --ar)
            [ "$#" -ge 2 ] || usage
            ar=$2
            shift 2
            ;;
        --src)
            [ "$#" -ge 2 ] || usage
            src=$2
            shift 2
            ;;
        --include)
            [ "$#" -ge 2 ] || usage
            include=$2
            shift 2
            ;;
        --out)
            [ "$#" -ge 2 ] || usage
            out=$2
            shift 2
            ;;
        --crt0)
            [ "$#" -ge 2 ] || usage
            crt0=$2
            shift 2
            ;;
        --heap)
            [ "$#" -ge 2 ] || usage
            heap=$2
            shift 2
            ;;
        --no-float)
            float=0
            shift
            ;;
        --werror)
            werror=1
            shift
            ;;
        --objects)
            [ "$#" -ge 2 ] || usage
            objects=$2
            shift 2
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

[ -n "$src" ] || usage
[ -n "$include" ] || usage
[ -n "$out" ] || usage

[ -d "$src" ] || {
    echo "$0: no such directory of sources: $src" >&2
    exit 1
}
[ -f "$src/malloc.c" ] || {
    echo "$0: $src is not components/libc/user/src (no malloc.c)" >&2
    exit 1
}
[ -f "$include/ulibc.h" ] || {
    echo "$0: $include has no ulibc.h" >&2
    exit 1
}
[ -f "$include/kernel/ksys.h" ] || {
    echo "$0: $include has no kernel/ksys.h (the ABI of the calls)" >&2
    exit 1
}

case "$heap" in
    ""|*[!0-9]*)
        echo "$0: --heap wants a number of bytes, not '$heap'" >&2
        exit 2
        ;;
esac

compiler_include=$($cc -print-file-name=include)
[ -n "$compiler_include" ] && [ -d "$compiler_include" ] || {
    echo "$0: $cc -print-file-name=include names no directory" >&2
    exit 1
}

cflags="-Os -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
        -march=x86-64 -fno-asynchronous-unwind-tables -ffunction-sections
        -fdata-sections -Wall -Wextra"

if [ "$werror" -eq 1 ]; then
    cflags="$cflags -Werror"
fi

if [ "$float" -eq 1 ]; then
    cflags="$cflags -DULIBC_WITH_FLOAT"
else
    cflags="$cflags -mgeneral-regs-only"
fi

# The objects of one build live somewhere temporary unless the caller names a
# directory, and a temporary one is removed on the way out: the archive and
# the crt are the output, not a tree of objects a person has to find again.
if [ -z "$objects" ]; then
    objects=$(mktemp -d) || exit 1
    trap 'rm -rf "$objects"' EXIT
fi
mkdir -p "$objects"

compile()
{
    # shellcheck disable=SC2086
    $cc $cflags -isystem "$include" -isystem "$compiler_include" \
        -DULIBC_HEAP_SIZE="${heap}u" -c "$1" -o "$2"
}

count=0
for source in "$src"/*.c; do
    stem=$(basename "$source" .c)

    if [ "$stem" = start ]; then
        # The entry of a program is not a member of the archive: it is the
        # crt the program is linked with, and only a caller that asked for it
        # gets the object.
        if [ -n "$crt0" ]; then
            mkdir -p "$(dirname "$crt0")"
            compile "$source" "$crt0"
            echo "crt0: $crt0"
        fi
        continue
    fi

    if [ "$float" -eq 0 ]; then
        skip=0
        for name in $K_NO_FLOAT_SKIP; do
            if [ "$stem" = "$name" ]; then
                skip=1
            fi
        done
        if [ "$skip" -eq 1 ]; then
            continue
        fi
    fi

    compile "$source" "$objects/$stem.o"
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "$0: no source was compiled from $src" >&2
    exit 1
fi

mkdir -p "$(dirname "$out")"
rm -f "$out"
# The deterministic mode (D) keeps the members' times and owners out of the
# archive, so two builds of one tree are one file; an ar without D still
# makes a working archive and only the bytes differ.
if ! $ar rcsD "$out" "$objects"/*.o 2>/dev/null; then
    rm -f "$out"
    $ar rcs "$out" "$objects"/*.o
fi

if [ "$float" -eq 1 ]; then
    kind="float"
else
    kind="integer"
fi
echo "library: $out ($count objects, $kind, arena $heap bytes)"
