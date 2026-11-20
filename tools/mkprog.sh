#!/bin/sh
#
# Micro OS — turn an object into a program image (the MOX1 format).
#
#     tools/mkprog.sh [--base <hex>] <object.o> <out.mox> <entry-symbol>
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
K_EXEC_VERSION=1

usage()
{
    echo "usage: $0 [--base <hex>] <object.o> <out.mox> <entry-symbol>" >&2
    exit 2
}

base=

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

[ -f "$object" ] || {
    echo "mkprog: no such file: $object" >&2
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# A section that the object does not have is an empty file, not an error.
extract()
{
    : > "$2"
    objcopy -O binary --only-section="$1" "$object" "$2" 2>/dev/null || true
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

section_address()
{
    objdump -h "$object" | awk -v name="$1" '$2 == name { print "0x" $4; exit }'
}

section_size()
{
    objdump -h "$object" | awk -v name="$1" '$2 == name { print "0x" $3; exit }'
}

extract .text "$work/text.bin"
extract .data "$work/data.bin"
extract .rodata "$work/rodata.bin"

text_address=$(section_address .text)
if [ -z "$text_address" ]; then
    echo "mkprog: $object has no .text" >&2
    exit 1
fi

symbol_address=$(nm "$object" | awk -v name="$symbol" '$NF == name { print "0x" $1; exit }')
if [ -z "$symbol_address" ]; then
    echo "mkprog: no symbol '$symbol' in $object" >&2
    exit 1
fi

# The address of a symbol of a relocatable object is relative to its section,
# so only the section table says whether the entry is code. The image carries
# .text and nothing else of the executable sections.
symbol_section=$(objdump -t "$object" | awk -v name="$symbol" '$NF == name { print $4; exit }')
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

text_aligned=$(align4 "$text_size")
data_aligned=$(align4 "$data_size")

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

    if [ "$data_size" -gt 0 ]; then
        data_address=$(section_address .data)
        expected_data=$((expected_text + text_aligned))
        if [ -z "$data_address" ] || [ "$((data_address))" -ne "$expected_data" ]; then
            printf "mkprog: %s has its data at %s, the window puts it at 0x%x\n" \
                "$object" "${data_address:-nowhere}" "$expected_data" >&2
            exit 1
        fi
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
    emit_le32 0
    cat "$work/text.bin"
    head -c $((text_aligned - text_size)) /dev/zero
    cat "$work/segment.bin"
    head -c $((data_aligned - data_size)) /dev/zero
} > "$output"

echo "mkprog: $output: text $text_aligned, data $data_aligned, bss $bss_size, entry $entry"
