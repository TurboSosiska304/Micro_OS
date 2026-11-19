OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start64)

/*
 * The 64-bit kernel of the port. It is linked at 16 MiB: the loader of the
 * port copies it there before the switch to long mode, and the identity map
 * of the first gigabyte makes the physical and the virtual address the same,
 * which is what the small memory model expects.
 */
KERNEL_BASE = 0x01000000;

SECTIONS
{
    . = KERNEL_BASE;

    .text : {
        /* The entry point first: the loader jumps to the base of the image. */
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
    }
}
