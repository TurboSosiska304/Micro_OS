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

    /*
     * The thread pointer of the C library. The compiler addresses a
     * _Thread_local of the library as an offset from %fs, so the port has to
     * say where the block is; the symbols are the whole of it, because the
     * machine has one space of tasks and no thread of its own.
     */
    .tdata : {
        *(.tdata .tdata.*)
    }

    .tbss : {
        *(.tbss .tbss.*)
    }

    /*
     * The two ends are taken from the sections and not from the counter: a
     * NOBITS section takes no room in the file, and the value of the counter
     * after it is not the end of the block the compiler addresses.
     */
    __tls_start = ADDR(.tdata);
    __tls_end = ADDR(.tbss) + SIZEOF(.tbss);

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
