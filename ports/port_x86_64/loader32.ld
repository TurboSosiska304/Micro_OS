OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

/*
 * The loader of the x86-64 port: a 32-bit multiboot image at one megabyte,
 * which is where QEMU puts it. It only has to reach the end of its own
 * switch of the processor, so everything is in one place.
 */
SECTIONS
{
    . = 0x00100000;

    .multiboot : {
        KEEP(*(.multiboot))
    }

    .text : {
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

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
    }
}
