#!/bin/sh
#
# Micro OS — turn an object into a program image (the MOX1 format).
#
#     tools/mkprog.sh [--base <hex>] [--data-base <hex>] [--arch x86-64|xtensa] \
#                     <object.o> <out.mox> <entry-symbol>
#
# The image is flat: the header of kernel/exec.h, then .text, then the
# initialised data (.data and .rodata). The program is not relocated, so the
# object has to be linked for the base the port names (k_port_exec_base);
# the entry is written as an offset from the start of the image, which is why
# the address of the symbol is taken apart from the address of .text.
#
# With --base the script checks that the link really used that window: the
# first address of .text has to be the base plus the header, and the data has
# to follow the padded text. Without it the object is taken as it is, for the
# callers that build an image and do not know the port.
#
# A port whose data is a window of its own (the board, ADR 0018) passes
# --data-base as well: the data no longer follows the text, so the script
# checks the address of .data against that number instead of the contiguous
# rule above. A program linked for the wrong data window would read and write
# memory that belongs to something else.
#
# The tools come from OBJCOPY, OBJDUMP and NM when a build names them, because
# a cross build has no host objcopy that can read its ELF; the defaults are
# the ones of the host (the x86-64 port).
#
# The machine of the image is written in the field `arch` of the header
# (offset 28): x86-64 is 0 and is the default, Xtensa (the board) is 1. The
# kernel refuses an image of another machine before it enters it, so a
# program built for the wrong one is ENOEXEC and not a jump into bytes that
# mean nothing there (ADR 0018).
#
# The sizes are rounded up to four bytes and the file is padded to match: the
# port copies segment by segment in words, and a size that is not a multiple
# of four would leave a tear at the end of a segment. Each segment is padded
# before the next one starts and not once at the end of the file: the header
# says that the data begins at the header plus the (padded) text, and a
# program is linked for that address. A single pad at the end of the file
# leaves the data one, two or three bytes short of where the program reads
# it - a shift that runs, prints the wrong bytes and looks like a bug in the
# program. The trailing padding after the data is allowed by the format and
# is ignored by the parser.
set -eu

K_EXEC_HEADER_SIZE=32
K_EXEC_MAGIC=0x31584f4d # the bytes 'M', 'O', 'X', '1' read little endian
K_EXEC_VERSION=2

# The numbers of the field `arch`, the header of kernel/exec.h.
K_EXEC_ARCH_X86_64=0
K_EXEC_ARCH_XTENSA=1

usage()
{
    echo "usage: $0 [--base <hex>] [--data-base <hex>]" >&2
    echo "       [--arch x86-64|xtensa] <object.o> <out.mox> <entry-symbol>" >&2
    exit 2
}

base=
data_base=
arch=x86-64
arch_number=$K_EXEC_ARCH_X86_64

while [ "$#" -gt 0 ]; do
    case "$1" in
        --base)
            [ "$#" -ge 2 ] || usage
            base=$2
            shift 2
            ;;
        --base=*)
            base=${1#--base=}
            shift
            ;;
        --data-base)
            [ "$#" -ge 2 ] || usage
            data_base=$2
            shift 2
            ;;
        --data-base=*)
            data_base=${1#--data-base=}
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

[ "$#" -eq 3 ] || usage

object=$1
output=$2
symbol=$3

# The machine of the image: the name of the port, the number of the header.
# A name this format does not know is refused before anything is written, so
# no image carries an arch the kernel would have to refuse later.
case "$arch" in
    x86-64)
        arch_number=$K_EXEC_ARCH_X86_64
        ;;
    xtensa)
        arch_number=$K_EXEC_ARCH_XTENSA
        ;;
    *)
        echo "mkprog: --arch wants x86-64 or xtensa, not '$arch'" >&2
        exit 2
        ;;
esac

if [ -n "$base" ]; then
    # The number is hexadecimal with its 0x, the way the port and the linker
    # script write it. The digits are checked first: a stray character would
    # otherwise reach the arithmetic and fail in a message about the shell.
    case "$base" in
        0x*|0X*) ;;
        *)
            echo "mkprog: --base wants a hexadecimal address with 0x, not '$base'" >&2
            exit 2
            ;;
    esac
    base_digits=${base#0x}
    base_digits=${base_digits#0X}
    case "$base_digits" in
        ""|*[!0-9a-fA-F]*)
            echo "mkprog: --base wants a hexadecimal address, not '$base'" >&2
            exit 2
            ;;
    esac
    base=$((base))
fi

if [ -n "$data_base" ]; then
    # The same shape of number, one window down: a program of the board has
    # its data in a window of its own and is linked for that address.
    case "$data_base" in
        0x*|0X*) ;;
        *)
            echo "mkprog: --data-base wants a hexadecimal address with 0x, not '$data_base'" >&2
            exit 2
            ;;
    esac
    data_base_digits=${data_base#0x}
    data_base_digits=${data_base_digits#0X}
    case "$data_base_digits" in
        ""|*[!0-9a-fA-F]*)
            echo "mkprog: --data-base wants a hexadecimal address, not '$data_base'" >&2
            exit 2
            ;;
    esac
    data_base=$((data_base))
fi

[ -f "$object" ] || {
    echo "mkprog: no such file: $object" >&2
    exit 1
}

# The tools of the build: a cross build names its own (the environment of
# ESP-IDF has no host objcopy that can read an Xtensa ELF).
OBJCOPY=${OBJCOPY:-objcopy}
OBJDUMP=${OBJDUMP:-objdump}
NM=${NM:-nm}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# A section that the object does not have is an empty file, not an error.
extract()
{
    : > "$2"
    "$OBJCOPY" -O binary --only-section="$1" "$object" "$2" 2>/dev/null || true
}

file_size()
{
    wc -c < "$1" | tr -d ' '
}

align4()
{
    value=$1
    echo $(( (value + 3) / 4 * 4 ))
}

# Sixteen bytes is the alignment of the data segment of a program: a constant
# and an arena with a `_Alignas(16)` are objects of that width, and the
# instruction that reads one (movaps, andps) faults on a lesser one. The text
# is a page because the port gives it a protection; the two are multiples of
# sixteen, so the header of the format stays a multiple of four.
align16()
{
    value=$1
    echo $(( (value + 15) / 16 * 16 ))
}

section_address()
{
    "$OBJDUMP" -h "$object" | awk -v name="$1" '$2 == name { print "0x" $4; exit }'
}

section_size()
{
    "$OBJDUMP" -h "$object" | awk -v name="$1" '$2 == name { print "0x" $3; exit }'
}

extract .text "$work/text.bin"
extract .data "$work/data.bin"
extract .rodata "$work/rodata.bin"

text_address=$(section_address .text)
if [ -z "$text_address" ]; then
    echo "mkprog: $object has no .text" >&2
    exit 1
fi

symbol_address=$("$NM" "$object" | awk -v name="$symbol" '$NF == name { print "0x" $1; exit }')
if [ -z "$symbol_address" ]; then
    echo "mkprog: no symbol '$symbol' in $object" >&2
    exit 1
fi

# The address of a symbol of a relocatable object is relative to its section,
# so only the section table says whether the entry is code. The image carries
# .text and nothing else of the executable sections.
symbol_section=$("$OBJDUMP" -t "$object" | awk -v name="$symbol" '$NF == name { print $4; exit }')
if [ "$symbol_section" != ".text" ]; then
    echo "mkprog: '$symbol' is not in .text (it is in '$symbol_section')" >&2
    exit 1
fi

text_size=$(file_size "$work/text.bin")
if [ "$text_size" -eq 0 ]; then
    echo "mkprog: the .text of $object is empty" >&2
    exit 1
fi

# .rodata goes with the data: it is initialised bytes, and the format has one
# segment of them.
cat "$work/data.bin" "$work/rodata.bin" > "$work/segment.bin"
data_size=$(file_size "$work/segment.bin")

bss_size=$(section_size .bss)
bss_size=$((${bss_size:-0}))

text_aligned=$(align16 "$text_size")
data_aligned=$(align16 "$data_size")

# The board is the exception: its two windows are not contiguous and its
# linker script pads the data and the bss to four bytes, the alignment of the
# pointers of the machine. A program of the board computes in integers only,
# so no object of eight or sixteen bytes asks for more. The x86-64 window is
# contiguous and a program with floating point has 16-byte constants that
# `movaps` and `andps` read: there the two sizes are padded to sixteen.
if [ -n "$data_base" ]; then
    text_aligned=$(align4 "$text_size")
    data_aligned=$(align4 "$data_size")
else
    # The linked addresses are the truth of the layout: the linker may align
    # the start of the bss (or of the data) to a boundary wider than sixteen
    # when an object asks for it, and the header has to name the gap the
    # program was linked with or the port clears the bss at another address.
    data_address=$(section_address .data)
    bss_address=$(section_address .bss)

    if [ -n "$data_address" ] && [ "$data_size" -gt 0 ]; then
        text_gap=$((data_address - text_address))
        if [ "$text_gap" -ge "$text_size" ]; then
            text_aligned=$text_gap
        fi
    elif [ -n "$bss_address" ] && [ "$bss_size" -gt 0 ]; then
        text_gap=$((bss_address - text_address))
        if [ "$text_gap" -ge "$text_size" ]; then
            text_aligned=$text_gap
        fi
    fi

    if [ -n "$data_address" ] && [ "$data_size" -gt 0 ] && [ -n "$bss_address" ] &&
       [ "$bss_size" -gt 0 ]; then
        data_gap=$((bss_address - data_address))
        if [ "$data_gap" -ge "$data_size" ]; then
            data_aligned=$data_gap
        fi
    fi
fi

# The optional base is the contract of a port: nothing relocates the image, so
# a program linked for another window would run at addresses that name
# something else here. The format puts the text right after the header and the
# data right after the padded text, so those are the addresses to compare, not
# just the base. A wrong window is refused here instead of being copied and
# blamed on the program.
if [ -n "$base" ]; then
    expected_text=$((base + K_EXEC_HEADER_SIZE))
    if [ "$((text_address))" -ne "$expected_text" ]; then
        printf "mkprog: %s is linked for 0x%x, the window starts at 0x%x (text at 0x%x)\n" \
            "$object" "$((text_address))" "$base" "$expected_text" >&2
        exit 1
    fi
fi

if [ -n "$data_base" ]; then
    # The port of the board puts the data in a window of its own, so the
    # contiguous rule below does not apply; what matters is that the data and
    # the bss are where the loader will copy and clear them.
    if [ "$data_size" -gt 0 ]; then
        data_address=$(section_address .data)
        if [ -z "$data_address" ]; then
            echo "mkprog: $object has data but no .data section" >&2
            exit 1
        fi

        if [ "$((data_address))" -ne "$data_base" ]; then
            printf "mkprog: %s has its data at 0x%x, the data window starts at 0x%x\n" \
                "$object" "$((data_address))" "$data_base" >&2
            exit 1
        fi
    elif [ "$bss_size" -gt 0 ]; then
        bss_address=$(section_address .bss)
        if [ -z "$bss_address" ] || [ "$((bss_address))" -ne "$data_base" ]; then
            printf "mkprog: %s has its bss at 0x%x, the data window starts at 0x%x\n" \
                "$object" "${bss_address:-0}" "$data_base" >&2
            exit 1
        fi
    fi

    if [ "$bss_size" -gt 0 ]; then
        bss_address=$(section_address .bss)
        expected_bss=$((data_base + data_aligned))
        if [ -z "$bss_address" ] || [ "$((bss_address))" -ne "$expected_bss" ]; then
            printf "mkprog: %s has its bss at 0x%x, the loader clears 0x%x\n" \
                "$object" "${bss_address:-0}" "$expected_bss" >&2
            exit 1
        fi
    fi
elif [ -n "$base" ]; then
    if [ "$data_size" -gt 0 ]; then
        data_address=$(section_address .data)
        if [ -z "$data_address" ]; then
            echo "mkprog: $object has data but no .data section" >&2
            exit 1
        fi

        # The linker may align the data past the padded text (a compiler is
        # free to ask for eight bytes for a merged string, and not every ld
        # obeys SUBALIGN the same way). The image can carry that gap as
        # padding of the text segment: the header says where the data starts,
        # and the port copies the zeros as part of the text.
        data_offset=$((data_address - expected_text))
        if [ "$data_offset" -lt "$text_size" ] ||
           [ $((data_offset % 4)) -ne 0 ] ||
           [ "$data_offset" -gt "$((text_aligned + 64))" ]; then
            printf "mkprog: %s has its data %d byte(s) after the text, the window cannot pad that\n" \
                "$object" "$data_offset" >&2
            exit 1
        fi

        text_aligned=$data_offset
    fi
fi

entry=$((symbol_address - text_address + K_EXEC_HEADER_SIZE))
entry_end=$((K_EXEC_HEADER_SIZE + text_size))
if [ "$entry" -lt "$K_EXEC_HEADER_SIZE" ] || [ "$entry" -ge "$entry_end" ]; then
    echo "mkprog: '$symbol' at $symbol_address is not inside .text" >&2
    exit 1
fi

# A field of the header, little endian. The bytes leave as the octal
# escapes of printf and not through a hex dumper: the script has to run on a
# bare build machine, and xxd is a part of an editor that nobody promised.
emit_le32()
{
    word=$1
    shift=0

    while [ "$shift" -lt 32 ]; do
        byte=$(( (word >> shift) & 0xff ))
        printf "\\$(printf '%03o' "$byte")"
        shift=$((shift + 8))
    done
}

{
    emit_le32 "$K_EXEC_MAGIC"
    emit_le32 "$K_EXEC_VERSION"
    emit_le32 0
    emit_le32 "$entry"
    emit_le32 "$text_aligned"
    emit_le32 "$data_aligned"
    emit_le32 "$bss_size"
    emit_le32 "$arch_number"
    cat "$work/text.bin"
    head -c $((text_aligned - text_size)) /dev/zero
    cat "$work/segment.bin"
    head -c $((data_aligned - data_size)) /dev/zero
} > "$output"

echo "mkprog: $output: $arch, text $text_aligned, data $data_aligned, bss $bss_size, entry $entry"
