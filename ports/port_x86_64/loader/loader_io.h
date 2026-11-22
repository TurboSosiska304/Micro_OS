/*
 * Micro OS — the console of the 32-bit loader.
 *
 * The verifier runs before the kernel has a console, but a refused image has
 * to say why on the machine a person is looking at: the loader writes COM1
 * and the last line of the VGA text screen, the two devices the kernel's
 * console will use a moment later. There is no formatting here, only a
 * string, because the loader has no libc and no heap.
 */
#ifndef MICRO_OS_PORT_X86_64_LOADER_LOADER_IO_H
#define MICRO_OS_PORT_X86_64_LOADER_LOADER_IO_H

void loader_serial_init(void);
void loader_puts(const char *text);

#endif /* MICRO_OS_PORT_X86_64_LOADER_LOADER_IO_H */
