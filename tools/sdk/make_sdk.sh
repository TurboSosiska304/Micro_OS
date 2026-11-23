#!/bin/sh
#
# Micro OS — assemble the SDK: headers, the library of a program and the link
# of the format, so that a program is built outside this tree.
#
#     tools/sdk/make_sdk.sh [--destdir <dir>] [--heap <bytes>] [--version <v>]
#
# The result is `<destdir>/microos-sdk/<version>/`:
#
#   VERSION                      the version of the kernel it was cut from
#   include/                     ulibc.h, the POSIX headers of the library,
#                                kernel/{ksys,exec,errno,types,vfs,task,signal}.h
#                                and prog.h (the raw door)
#   lib/libmicroos_user.a        the library of a program, built here
#   lib/crt0.o                   its entry, `_start` (src/start.c)
#   lib/program.ld               the addresses of the window (MOX1)
#   bin/mkprog.sh                the sections -> the image (the format)
#   bin/link_program.sh          the link of the window, which calls mkprog
#   bin/microos_sign             the host tool of the signature (ADR 0020)
#   share/microos-toolchain.cmake  the CMake toolchain of a program
#   share/build-libc.sh          the recipe that built lib/libmicroos_user.a
#   share/src/libc/              the sources of that library (MIT)
#   share/examples/hello/        a program that builds with nothing else
#   share/README.md              how to build a program with it
#
# Nothing of this directory is a second copy of the tree: every file is copied
# or built from the tree, and the SDK is the interface of the tree and not a
# fork of it. The version is the one of components/kernel/include/kernel/
# config.h, which is the version of `kernel_version()`; the directory is named
# after it so that two SDKs of two releases do not overwrite each other.
#
# The library is built from the copy of the sources inside the SDK and not
# from components/libc/user/src: what travels is what was built, and a person
# who rebuilds it with `share/build-libc.sh` compiles the same files. The
# build is the recipe of a guest object of the port (see build_libc.sh), with
# -Werror: the sources of this tree are warning-free and the SDK of a release
# says so.
#
# --heap is the arena of `malloc` (the default is 256 KiB, the default of the
# library); --version overrides the version of the directory, which a release
# machine uses when the tree is mid-way between two `config.h` values.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
config="$root/components/kernel/include/kernel/config.h"

destdir=dist
heap=262144
version=

usage()
{
    echo "usage: $0 [--destdir <dir>] [--heap <bytes>] [--version <v>]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --destdir)
            [ "$#" -ge 2 ] || usage
            destdir=$2
            shift 2
            ;;
        --heap)
            [ "$#" -ge 2 ] || usage
            heap=$2
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] || usage
            version=$2
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

[ -f "$config" ] || {
    echo "$0: no $config: is this a tree of Micro OS?" >&2
    exit 1
}

field()
{
    sed -n "s/^#define $1[[:space:]]\{1,\}\(.*\)\$/\1/p" "$config" | head -n 1
}

if [ -z "$version" ]; then
    major=$(field KERNEL_VERSION_MAJOR)
    minor=$(field KERNEL_VERSION_MINOR)
    patch=$(field KERNEL_VERSION_PATCH)
    suffix=$(field KERNEL_VERSION_SUFFIX)

    [ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || {
        echo "$0: $config does not name the version of the kernel" >&2
        exit 1
    }

    # The suffix is a C literal of the header ("-rc7" or ""): the quotes are
    # the shape of the file and not a part of the version of the directory.
    suffix=$(printf '%s' "$suffix" | tr -d '"')
    version="$major.$minor.$patch$suffix"
fi

case "$version" in
    ""|*/*)
        echo "$0: '$version' is not a version" >&2
        exit 1
        ;;
esac

case "$heap" in
    ""|*[!0-9]*)
        echo "$0: --heap wants a number of bytes, not '$heap'" >&2
        exit 2
        ;;
esac

mkdir -p "$destdir"
destdir=$(cd "$destdir" && pwd)
target="$destdir/microos-sdk/$version"

# A build replaces the SDK of the same version: a file left from an older
# build of the tree would be a header of a release beside a library of
# another. The path is checked first so that a mistake of a variable cannot
# remove something that is not an SDK.
case "$target" in
    */microos-sdk/"$version") ;;
    *)
        echo "$0: refusing to replace '$target'" >&2
        exit 1
        ;;
esac
rm -rf "$target"

mkdir -p "$target/include/kernel" "$target/lib" "$target/bin" "$target/share"

# --- the headers of a program ----------------------------------------------
#
# The headers of the library (`ulibc.h`, `<stdio.h>`, `<stdlib.h>`, ...) and
# the headers of the ABI, which the library includes as `kernel/ksys.h`. The
# ABI header carries the VFS structures and the task record with it, so the
# closure is not one file: ksys.h -> signal.h, task.h -> vfs.h, types.h.
cp -R "$root/components/libc/user/include/." "$target/include/"
for header in types.h vfs.h task.h signal.h ksys.h errno.h exec.h; do
    cp "$root/components/kernel/include/kernel/$header" "$target/include/kernel/$header"
done

# The raw door (int $0x80) for a program that does not use the library at all,
# and the linker script of the window.
cp "$root/ports/port_x86_64/programs/prog.h" "$target/include/prog.h"
cp "$root/ports/port_x86_64/programs/program.ld" "$target/lib/program.ld"

# --- the sources and the library -------------------------------------------

mkdir -p "$target/share/src/libc"
cp -R "$root/components/libc/user/src/." "$target/share/src/libc/"
cp "$root/tools/sdk/build_libc.sh" "$target/share/build-libc.sh"
chmod 755 "$target/share/build-libc.sh"

sh "$target/share/build-libc.sh" \
    --src "$target/share/src/libc" \
    --include "$target/include" \
    --out "$target/lib/libmicroos_user.a" \
    --crt0 "$target/lib/crt0.o" \
    --heap "$heap" \
    --werror

# --- the tools of the link --------------------------------------------------

cp "$root/tools/mkprog.sh" "$target/bin/mkprog.sh"
cp "$root/tools/link_program.sh" "$target/bin/link_program.sh"
chmod 755 "$target/bin/mkprog.sh" "$target/bin/link_program.sh"

# The tool of the signature is a program of the host and not of the machine
# (ADR 0020): it is compiled by the host compiler with the host libc, and it
# signs a `.mox` of a package the way it signs the image of the kernel.
${CC:-cc} -O2 -no-pie -o "$target/bin/microos_sign" \
    "$root/tools/microos_sign.c" \
    "$root/ports/port_x86_64/loader/sha256.c" \
    "$root/ports/port_x86_64/loader/ed25519.c" \
    "$root/ports/port_x86_64/loader/sign_image.c" \
    -I "$root/ports/port_x86_64/loader"

# --- the share of a build ---------------------------------------------------

cp "$root/tools/sdk/microos-toolchain.cmake" "$target/share/microos-toolchain.cmake"
cp "$root/tools/sdk/microos-project.cmake" "$target/share/microos-project.cmake"
mkdir -p "$target/share/examples"
cp -R "$root/examples/hello" "$target/share/examples/hello"
sed -e "s/@VERSION@/$version/g" -e "s/@HEAP@/$heap/g" \
    "$root/tools/sdk/README.md.in" > "$target/share/README.md"
printf '%s\n' "$version" > "$target/VERSION"

# --- what was made ----------------------------------------------------------

headers=$(find "$target/include" -name '*.h' | wc -l)
echo "sdk: $target"
echo "sdk: $headers headers, library $(wc -c < "$target/lib/libmicroos_user.a") bytes," \
    "crt0 $(wc -c < "$target/lib/crt0.o") bytes"
echo "sdk: cmake -B build -S <program> -DCMAKE_TOOLCHAIN_FILE=$target/share/microos-toolchain.cmake"
