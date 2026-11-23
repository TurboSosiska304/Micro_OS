#!/bin/sh
#
# Micro OS — link a program at the window of its port and make the image.
#
#     tools/link_program.sh --cc <command> --script <program.ld> \
#         --entry <symbol> --out <file.mox> --base <hex> \
#         [--arch x86-64|xtensa] [--mkprog <mkprog.sh>] [--elf <file.elf>] \
#         [-- <object.o> <library.a> ... <flags of the program>]
#
# A program of the x86-64 port is not relocated: the port copies the image to
# the start of its window and jumps into it, so the text has to be linked at
# the base of the window plus the header of the MOX1 format (32 bytes). The
# address is not a number every linker can be trusted with: the script of the
# format names it as the symbol __image_text, this wrapper passes the value
# with --defsym, and then *reads back* the address of .text and links again
# with the value the linker really used. A linker of another version, of
# another distribution or of a container that aligns the section differently
# is therefore corrected by measurement instead of being trusted (binutils
# 2.38 moves the text 32 bytes up; 2.47 keeps it).
#
# The last step is tools/mkprog.sh, which takes the sections apart and writes
# the image; it refuses an object whose text is not at the base plus the
# header, which is the check this wrapper exists to satisfy.
#
# The flags of the format are here and not in the callers: -nostdlib and
# -static (the program carries its own runtime and no host libc), -no-pie
# (the window is an absolute address), --build-id=none (the image is the
# sections and a note would be a section), --no-check-sections (the script
# places a section past the headers a linker may want to map) and -N (one
# writable region, because the port copies the image in one piece). The
# `--` list carries what belongs to one program: its objects, its archives
# and the flags of its own link (-Wl,--gc-sections).
#
# The compiler is a command and not a path, because a build writes "gcc" or
# "ccache gcc": the words are split the way the shell splits a command, and
# the `$CC` of the environment is the default.
set -eu

K_EXEC_HEADER_SIZE=32

usage()
{
    echo "usage: $0 --cc <command> --script <program.ld> --entry <symbol>" >&2
    echo "       --out <file.mox> --base <hex> [--arch x86-64|xtensa]" >&2
    echo "       [--mkprog <mkprog.sh>] [--elf <file.elf>]" >&2
    echo "       [-- <object.o> ... <library.a> ... <link flags>]" >&2
    exit 2
}

cc=${CC:-cc}
script=
entry=
out=
base=
arch=x86-64
mkprog=
elf=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cc)
            [ "$#" -ge 2 ] || usage
            cc=$2
            shift 2
            ;;
        --cc=*)
            cc=${1#--cc=}
            shift
            ;;
        --script)
            [ "$#" -ge 2 ] || usage
            script=$2
            shift 2
            ;;
        --script=*)
            script=${1#--script=}
            shift
            ;;
        --entry)
            [ "$#" -ge 2 ] || usage
            entry=$2
            shift 2
            ;;
        --entry=*)
            entry=${1#--entry=}
            shift
            ;;
        --out)
            [ "$#" -ge 2 ] || usage
            out=$2
            shift 2
            ;;
        --out=*)
            out=${1#--out=}
            shift
            ;;
        --base)
            [ "$#" -ge 2 ] || usage
            base=$2
            shift 2
            ;;
        --base=*)
            base=${1#--base=}
            shift
            ;;
        --arch)
            [ "$#" -ge 2 ] || usage
            arch=$2
            shift 2
            ;;
        --arch=*)
            arch=${1#--arch=}
            shift
            ;;
        --mkprog)
            [ "$#" -ge 2 ] || usage
            mkprog=$2
            shift 2
            ;;
        --mkprog=*)
            mkprog=${1#--mkprog=}
            shift
            ;;
        --elf)
            [ "$#" -ge 2 ] || usage
            elf=$2
            shift 2
            ;;
        --elf=*)
            elf=${1#--elf=}
            shift
            ;;
        --)
            shift
            break
            ;;
        -*)
            usage
            ;;
        *)
            break
            ;;
    esac
done

[ -n "$cc" ] || usage
[ -n "$script" ] || usage
[ -n "$entry" ] || usage
[ -n "$out" ] || usage
[ -n "$base" ] || usage

# The number is hexadecimal with its 0x, the way the script of the format and
# mkprog.sh write it. The digits are checked first: a stray character would
# otherwise reach the arithmetic and fail in a message about the shell.
case "$base" in
    0x*|0X*) ;;
    *)
        echo "link: --base wants a hexadecimal address with 0x, not '$base'" >&2
        exit 2
        ;;
esac
base_digits=${base#0x}
base_digits=${base_digits#0X}
case "$base_digits" in
    ""|*[!0-9a-fA-F]*)
        echo "link: --base wants a hexadecimal address, not '$base'" >&2
        exit 2
        ;;
esac

[ -f "$script" ] || {
    echo "link: no such linker script: $script" >&2
    exit 1
}

# The tools of the build: mkprog.sh is the one of this tree unless a caller
# names another, and the ELF of the link is a file of the build named after
# the image (the tree has always kept it, for the objdump of a bad link).
if [ -z "$mkprog" ]; then
    mkprog=$(dirname "$0")/mkprog.sh
fi
[ -f "$mkprog" ] || {
    echo "link: no such mkprog: $mkprog" >&2
    exit 1
}

if [ -z "$elf" ]; then
    elf=${out%.mox}.elf
fi

OBJDUMP=${OBJDUMP:-objdump}

# The link for one value of __image_text. The value goes to the script as a
# definition of the symbol, not as a number the script computes: a linker
# that moves the text for its own reasons (the alignment of an input section,
# the headers of the file in the first segment) moves it from the value it is
# given, and the next attempt subtracts that move.
link_at()
{
    target=$1
    shift

    # The command of the build is a string: the shell splits it into words
    # here, and the arguments of this call follow it untouched.
    # shellcheck disable=SC2086
    $cc -nostdlib -static -no-pie \
        -Wl,--build-id=none -Wl,--no-check-sections \
        -Wl,-z,noseparate-code -Wl,-N \
        -Wl,-e,"$entry" -Wl,--defsym=__image_text="$target" \
        -T "$script" -o "$elf" "$@"
}

text_address()
{
    "$OBJDUMP" -h "$elf" 2>/dev/null | awk '$2 == ".text" { print "0x" $4; exit }'
}

# The address the format needs: the header of 32 bytes is in front of the
# first byte of the text, and the port copies the image to the start of the
# window.
expected=$((base + K_EXEC_HEADER_SIZE))

attempt=1
target=$expected
previous_text=
text=
delta=

while : ; do
    # The number of the attempt goes to the message of a link that fails; a
    # linker error means the command or an object of the caller is wrong, and
    # another attempt with another address would repeat it.
    if ! link_at "$target" "$@"; then
        printf "link: %s: the linker failed (attempt %d, __image_text=0x%x)\n" \
            "$out" "$attempt" "$target" >&2
        exit 1
    fi

    text=$(text_address)
    if [ -z "$text" ]; then
        echo "link: $elf has no .text to measure" >&2
        exit 1
    fi

    delta=$((text - expected))
    if [ "$delta" -eq 0 ]; then
        break
    fi

    # Three links are the budget: the first is the value of the format, the
    # second is that value minus the move the linker made, and the third is
    # there for a linker that applies the move in more than one step. A
    # program whose text is still away from the window after three links is
    # not a program this wrapper can place.
    [ "$attempt" -lt 3 ] || break

    # A linker that answers the same address to two different requests is not
    # going to answer the address that is wanted: the fourth attempt would be
    # the third one again, and the failure below names both numbers.
    if [ -n "$previous_text" ] && [ "$((text))" -eq "$((previous_text))" ]; then
        break
    fi

    previous_text=$text
    target=$((expected - delta))
    attempt=$((attempt + 1))
done

if [ "$delta" -ne 0 ]; then
    printf "link: %s: .text is at 0x%x, the image of the window 0x%x needs 0x%x\n" \
        "$elf" "$((text))" "$((base))" "$expected" >&2
    printf "link: the delta is %d after %d link(s): the text, the data and every\n" \
        "$delta" "$attempt" >&2
    echo "link: address the program computes would name something else" >&2
    exit 1
fi

printf "link: %s: text 0x%x, delta %d\n" "$out" "$((text))" 0

# The sections of the ELF are the image: mkprog.sh checks the window itself
# and writes the header of the format.
sh "$mkprog" --base "$base" --arch "$arch" "$elf" "$out" "$entry"
