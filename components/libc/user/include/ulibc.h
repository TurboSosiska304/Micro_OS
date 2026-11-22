/*
 * Micro OS — the library of a program.
 *
 * This is not the library of the kernel (components/libc/src): that one is
 * linked into the kernel and calls the wrappers of `ksys_*`, which a program
 * cannot reach. Here every function is a thin layer over the one door a
 * program has - `int $0x80` on x86-64 (ADR 0006, ADR 0016), the function the
 * port of the board hands to _start on the ESP32 (ADR 0018) - and nothing
 * else.
 *
 * The numbers of the calls and the structures they speak come from the same
 * header the kernel is compiled against (kernel/ksys.h, which carries the VFS
 * structures with it), so a number or a field can never drift between the two
 * sides of the door.
 *
 * The entry of a program of x86-64 is the C entry with three arguments (ADR
 * 0016):
 *
 *     int main(int argc, char *const argv[], char *const envp[]);
 *
 * A program linked against this library starts in `_start` (src/start.c),
 * which calls main() and then exit() with its result; a program does not
 * return from main() into anything, because there is no kernel frame under
 * it. A program that ends with `ret` instead faults on address zero, which is
 * the guard the port leaves at the top of the stack.
 *
 * On the board `_start` takes one argument more, the door, and the build
 * defines MOX_ARCH_XTENSA so that src/start.c and src/syscall.c keep it. The
 * `main` of the program is the same function, and the numbers of the calls
 * are the same numbers.
 *
 * The result of a call is the result of the system call: a negative number is
 * the error with its minus sign (-2 is ENOENT), exactly as kernel/errno.h
 * names it. The library does not keep an errno of its own; a program that
 * wants the POSIX shape can use the sign.
 */
#ifndef MICRO_OS_LIBC_USER_ULIBC_H
#define MICRO_OS_LIBC_USER_ULIBC_H

#include <stdarg.h>
#include <stddef.h>

/* The numbers and the structures of the calls, from the kernel's own header. */
#include "kernel/ksys.h"

/* The descriptors a program knows without opening them. The console of the
 * port is 1 and 2. Descriptor 0 is the input of a program: the shell does not
 * pass its own table to a program yet (docs/debts.md), so it is open only
 * when the program opened it - which is the first descriptor a fresh table
 * gives out - and a read of it answers EBADF otherwise. */
#define ULIBC_STDIN 0
#define ULIBC_STDOUT 1
#define ULIBC_STDERR 2

/* The flags of open, the numbers of POSIX; open() maps them to the ABI. */
#define ULIBC_O_RDONLY 0
#define ULIBC_O_WRONLY 1
#define ULIBC_O_RDWR 2
#define ULIBC_O_CREAT 0100
#define ULIBC_O_TRUNC 01000
#define ULIBC_O_APPEND 02000
#define ULIBC_O_DIRECTORY 0200000

/* The whence of lseek, the numbers of POSIX. */
#define ULIBC_SEEK_SET 0
#define ULIBC_SEEK_CUR 1
#define ULIBC_SEEK_END 2

#define ULIBC_EXIT_SUCCESS 0
#define ULIBC_EXIT_FAILURE 1

/* --- the calls --- */

int open(const char *path, int flags, ...);
int close(int fd);
long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);
long lseek(int fd, long offset, int whence);
/*
 * The next entries of a directory opened with ULIBC_O_DIRECTORY. `count` is a
 * number of bytes, not of entries, and has to be a whole number of them; the
 * answer is the number of bytes written or a negative error. The end of the
 * directory is an empty answer, not an error.
 */
long ulibc_readdir(int fd, struct vfs_dirent *entries, size_t count);

/*
 * The objects of the tree: the information of a path, a directory, a name
 * removed. `stat` answers 0 and fills the structure, `mkdir` and `unlink`
 * answer 0; a negative number is the error of the call.
 */
int ulibc_stat_call(const char *path, struct vfs_stat *info);
int mkdir(const char *path, ...);
int unlink(const char *path);

/*
 * The current directory of the task, in the buffer of the caller: the path
 * the shell had when it started the program. The answer is `buf`, or NULL
 * when the call refused; the text is terminated in every case.
 */
char *getcwd(char *buf, size_t size);

int getpid(void);
long uptime(void);

/*
 * Make this task a program: the path is read through the VFS, the mode and
 * the image are checked, and a new task runs the program while this one waits
 * for it (ADR 0016). The environment is a vector of "NAME=VALUE" words; NULL
 * is the environment of the system. The answer is 0 when the program ended,
 * and a negative error when it did not start; the status of the program itself
 * goes to whoever waits for this task.
 */
int execve(const char *path, char *const argv[], char *const envp[]);

/*
 * End the program. The kernel takes the task down and the caller of execve
 * waits for it; the low byte of the code is the status it sees. This call
 * does not return.
 */
void exit(int code) __attribute__((noreturn));

/* --- the strings --- */

size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
/*
 * The first place of `needle` in `text`, or NULL. An empty needle is the
 * text itself, as in the library of a Unix-like system.
 */
char *strstr(const char *text, const char *needle);
char *strcpy(char *dest, const char *source);
char *strncpy(char *dest, const char *source, size_t count);
char *strcat(char *dest, const char *source);
char *strchr(const char *text, int byte);
char *strrchr(const char *text, int byte);
char *strdup(const char *text);
char *strndup(const char *text, size_t count);
size_t strspn(const char *text, const char *set);
size_t strcspn(const char *text, const char *set);
char *strpbrk(const char *text, const char *set);
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t count);
char *strcasestr(const char *text, const char *needle);
void *memcpy(void *dest, const void *source, size_t count);
void *memmove(void *dest, const void *source, size_t count);
void *memset(void *dest, int byte, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *memory, int byte, size_t count);

/* --- the allocation of a program --- */

/*
 * The arena lives in the bss of the program, not in the kernel: a block of
 * `KSYS_MALLOC` is memory of ring 0 and a program could not read it. The
 * size is a compile-time setting (`ULIBC_HEAP_SIZE`), 256 KiB by default,
 * because the bss of an image is capped at a megabyte by the format.
 */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *block, size_t size);
void free(void *block);

/* --- the error number --- */

int *__ulibc_errno(void);
#define errno (*__ulibc_errno())
char *strerror(int code);

/* --- the files of the VFS --- */

typedef struct ulibc_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fgetc(FILE *stream);
int fputc(int byte, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int fputs(const char *text, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);

/* --- the console --- */

/*
 * The conversions of the formatter: %d, %i, %u, %o, %x, %X, %c, %s, %p and
 * %%. A width and the flags '-' and '0' are honoured; the length is 'l' or
 * 'll' for a long or a long long, 'z' for a size_t, and the answer is the
 * number of bytes written or a
 * negative error. The text goes to the console through write(1, ...), in
 * pieces of a fixed buffer, so no allocation is involved. snprintf and
 * vsnprintf write the same text to a buffer of the caller, with the length
 * the whole text would have had, as the C library promises.
 */
int printf(const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int puts(const char *text);

#endif /* MICRO_OS_LIBC_USER_ULIBC_H */
