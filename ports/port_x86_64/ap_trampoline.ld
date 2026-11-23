/*
 * The AP trampoline of the x86-64 port, linked at the physical page a
 * start-up IPI names (ADR 0012). The image is a flat binary: the loader
 * copies it to 0x7000 before it enters the kernel, and the BSP fills the
 * parameter block at 0x7F00 before every SIPI.
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(ap_start)

SECTIONS
{
    . = 0x7000;

    .text : {
        *(.text .text.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
    }
}
