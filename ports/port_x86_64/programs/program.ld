/*
 * Micro OS — the addresses of a program of the x86-64 port (the MOX1 format).
 *
 * A program is not relocated: the port copies the image to the start of its
 * window and jumps into it, so the code and the data have to be linked for the
 * places they will really occupy there. The image is the header of 32 bytes
 * (kernel/exec.h) followed by the text, so the first byte of the text lands at
 * the base of the window plus the header, and the data follows the text.
 *
 * tools/mkprog.sh takes the sections apart and rebuilds them in that order:
 * .text, then .data together with .rodata, then nothing (the bss is not in the
 * file). The output sections below are laid out to match, byte for byte, with
 * four byte boundaries: a section that lands one byte away from where the
 * script says it is would send every reference of the program to the wrong
 * place, and the failure would look like a bug in the program.
 *
 * The base is the number port_exec.c uses (PORT_EXEC_BASE); the two are two
 * halves of one contract of the port and they have to be changed together.
 *
 * The entry symbol is not named here: it comes from the build (`-e`, the same
 * symbol tools/mkprog.sh is given), so that this script serves every program
 * and the format stays the only place that decides where a program starts.
 */
OUTPUT_FORMAT(elf64-x86-64)

/*
 * Two segments with the permissions they are: the text is read and executed,
 * the data is read and written. The image is flat and the processor runs it
 * through the identity map of the loader, so this is the shape of the file and
 * not a protection; it keeps the linker from warning about a segment that is
 * all three at once.
 */
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* k_port_exec_base() + the header of the format. */
    . = 0x02000020;

    .text : SUBALIGN(4) { *(.text .text.*) } :text

    /*
     * The initialised data and the read-only data are one segment in the
     * format, in the order mkprog.sh concatenates them: .data first.
     */
    . = ALIGN(4);
    .data : { *(.data .data.*) *(.rodata .rodata.*) } :data

    /*
     * The bss is not in the image: the port clears this many bytes right
     * after the data. SUBALIGN(4) is what makes the address of the section
     * here the address the port will clear, because the format has no
     * alignment of its own.
     */
    . = ALIGN(4);
    .bss (NOLOAD) : SUBALIGN(4) { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
    }
}
