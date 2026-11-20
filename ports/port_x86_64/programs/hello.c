/*
 * Micro OS — hello, the first program of the system that is a file.
 *
 * It is built by the port (ports/port_x86_64/CMakeLists.txt): the object of
 * this file is linked at the address of the window of the programs, turned
 * into an image of the MOX1 format by tools/mkprog.sh and embedded in the
 * kernel. At the boot the image is written to /bin/hello of the RAM volume,
 * read back through the VFS and run, so the bytes that are executed have been
 * through a file and not through a variable of the kernel.
 *
 * Nothing of the kernel is available here and there is no libc: a program
 * talks to the machine and to the system through what it is given. For this
 * one that is COM1, the same console the kernel uses, written through the
 * port of the UART - which is why the port leaves the program the interrupts
 * off and a stack of its own (see port_exec.c).
 *
 * The program returns, and the port takes it back. A program of the system
 * ends in a halt or in the exit call of a process, neither of which exists
 * yet; a demonstration that comes back is also what lets the shell of the
 * smoke test start at all.
 */
typedef unsigned char u8_t;
typedef unsigned short u16_t;

#define COM1 0x3F8
#define COM1_LSR 0x3FD /* line status: bit 5 "the transmitter can take a byte" */

static inline void outb(u16_t port, u8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8_t inb(u16_t port)
{
    u8_t value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

static void putc(char c)
{
    while ((inb(COM1_LSR) & 0x20u) == 0) {
    }

    outb(COM1, (u8_t)c);
}

static void puts(const char *text)
{
    while (*text != '\0') {
        putc(*text++);
    }
}

/*
 * The entry of the format: the port enters a program with the count and the
 * vector of the arguments in the registers of the usual ABI (see
 * k_port_exec_enter). This one has nothing to do with them, and the line ends
 * with a carriage return and a newline because no kernel is translating it.
 */
void hello_main(int argc, char *const argv[])
{
    (void)argc;
    (void)argv;

    puts("PROGRAM: hello from /bin\r\n");
}
