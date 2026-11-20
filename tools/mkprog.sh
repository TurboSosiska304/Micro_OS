#!/bin/sh
#
# Micro OS — turn an object into a program image (the MOX1 format).
#
#     tools/mkprog.sh <object.o> <out.mox> <entry-symbol>
#
# The image is flat: the header of kernel/exec.h, then .text, then the
# initialised data (.data and .rodata). The program is not relocated, so the
# object has to be linked for the base the port names (k_port_exec_base);
# the entry is written as an offset from the start of the image, which is why
# the address of the symbol is taken apart from the address of .text.
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
    echo "usage: $0 <object.o> <out.mox> <entry-symbol>" >&2
    exit 2
}

[ "$#" -eq 3 ] || usage

object=$1
output=$2
symbol=$3

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
