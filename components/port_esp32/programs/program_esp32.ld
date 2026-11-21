/*
 * Micro OS — the addresses of a program of the board (ADR 0018).
 *
 * The machine cannot relocate an image, so a program is linked for the two
 * places where the port puts it:
 *
 *   the text  at PORT_EXEC_TEXT_BASE (0x4009E000) plus the header of the
 *             format, in the instruction RAM below the top of the segment;
 *   the data  at PORT_EXEC_DATA_BASE (0x3FFCC000), the top of the static DRAM
 *             segment, which the build keeps out of the heap.
 *
 * The two windows are not contiguous, and that is the shape of the hardware:
 * the instruction RAM of the ESP32 is only 32-bit accessible for data, so a C
 * program that reads its strings byte by byte cannot live in the same window
 * as its code. The text and its literal pools go to IRAM; the data, the bss
 * and the strings of the library go to DRAM.
 *
 * tools/mkprog.sh --base 0x4009E000 --data-base 0x3FFCC000 checks both
 * addresses against this script before an image is written. The numbers are
 * also in components/port_esp32/src/port_exec.c and in the assertion of
 * components/port_esp32/exec_windows.ld: the three have to change together.
 *
 * The base is the start of the window, and the header of 32 bytes is not
 * copied to either window; the text begins right after it, which is why the
 * origin of the text region is the window plus the header. mkprog.sh writes
 * the entry as an offset from the start of the image, and the port adds it to
 * the window; the number in the file and the address here are two halves of
 * one contract.
 */
OUTPUT_FORMAT(elf32-xtensa-le)

MEMORY
{
    /* The 8 KiB window of the text, less the header of the format. */
    text (RX) : ORIGIN = 0x4009E020, LENGTH = 0x1FE0

    /* The 8 KiB window of the data, the bss and the vectors of the program. */
    data (RW) : ORIGIN = 0x3FFCC000, LENGTH = 0x2000
}

SECTIONS
{
    /*
     * The code and the literal pools of `l32r`: with -mtext-section-literals
     * the literals live in .literal and are read through the instruction
     * address, which is what the IRAM window can do. SUBALIGN(4) keeps a
     * literal on the four-byte boundary the instruction requires.
     */
    .text : SUBALIGN(4)
    {
        *(.text .text.*)
        *(.literal .literal.*)
    } > text

    /*
     * The initialised data and the read-only data are one segment in the
     * format, in the order mkprog.sh concatenates them: .data first. The port
     * copies them to the base of the data window.
     */
    .data : SUBALIGN(4)
    {
        *(.data .data.*)
        *(.rodata .rodata.*)
        *(.sdata .sdata.*)
        *(.srodata .srodata.*)
    } > data

    /*
     * The bss is not in the image: the port clears this many bytes at the
     * address the script gives it. The four-byte boundary before it is the
     * one the format pads the data to.
     */
    . = ALIGN(4);
    .bss (NOLOAD) : SUBALIGN(4)
    {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } > data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
        *(.xtensa.info)
        *(.xt.lit)
        *(.xt.prop)
    }
}
