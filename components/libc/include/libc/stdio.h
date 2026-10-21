/*
 * Micro OS — the C library of the system.
 *
 * The library is thin: every function is a call of the ABI of the system
 * calls, so that a program of the board talks to the kernel the same way as
 * any other program. The names are the usual ones, the behaviour is the
 * usual one, the errors are the codes of POSIX.
 *
 * The library does not know anything about the kernel: it only knows the
 * contract of the calls (see kernel/ksys.h and docs/syscall-abi.md).
 */
#ifndef MICRO_OS_LIBC_STDIO_H
#define MICRO_OS_LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * The formatting.
 *
 *   %d %i   the signed number
 *   %u      the unsigned number
 *   %x %X   the number in the base 16
 *   %o      the number in the base 8
 *   %c      the character
 *   %s      the string
 *   %p      the pointer
 *   %f %F   the number with a point
 *   %e %E   the number with an exponent
 *   %g %G   the number, the shortest of the two forms
 *   %%      the percent
 *
 * The flags: '-' (to the left), '+' (the sign of the positive numbers),
 * ' ' (a space before the positive numbers), '0' (the zeros in front),
 * '#' (the prefix of the base). The width and the precision are written
 * as usual, '*' takes them from the arguments.
 */
#if defined(__GNUC__)
/*
 * The attributes of the format: the compiler checks the format strings of
 * the programs the same way as it checks the ones of the usual library.
 */
int microos_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(__printf__, 3, 4)));
int microos_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __attribute__((format(__printf__, 3, 0)));
int microos_printf(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
int microos_vprintf(const char *fmt, va_list ap) __attribute__((format(__printf__, 1, 0)));
int microos_fprintf(int fd, const char *fmt, ...) __attribute__((format(__printf__, 2, 3)));
#else
int microos_snprintf(char *buf, size_t size, const char *fmt, ...);
int microos_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int microos_printf(const char *fmt, ...);
int microos_vprintf(const char *fmt, va_list ap);
int microos_fprintf(int fd, const char *fmt, ...);
#endif
int microos_puts(const char *text);
int microos_putchar(int ch);

/*
 * The usual names of the library. The functions keep their own names in
 * the binary (microos_*), so that the library of the compiler and this one
 * never fight for the same symbol.
 */
#define printf microos_printf
#define vprintf microos_vprintf
#define snprintf microos_snprintf
#define vsnprintf microos_vsnprintf
#define fprintf microos_fprintf
#define puts microos_puts
#define putchar microos_putchar

#endif /* MICRO_OS_LIBC_STDIO_H */
