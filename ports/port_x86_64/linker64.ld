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

    /*
     * The three regions of the kernel are separated by whole pages, because
     * src/paging.c gives each of them its own protection: the text is
     * readable and executable, the rodata is read-only and not executable,
     * and the data, the stacks and the heap are writable and not executable.
     * One page cannot be two of those at once, so a section does not share a
     * page with the next one.
     */
    __text_start = ADDR(.text);
    __text_end = ADDR(.text) + SIZEOF(.text);

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    __rodata_start = ADDR(.rodata);
    __rodata_end = ADDR(.rodata) + SIZEOF(.rodata);

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    __data_start = ADDR(.data);

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
    __tls_data_end = ADDR(.tbss);
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
