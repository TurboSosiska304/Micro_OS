#ifndef MICRO_OS_COMPAT_STDIO_H
#define MICRO_OS_COMPAT_STDIO_H
#include <stddef.h>
typedef struct _microos_FILE FILE;
extern FILE *stderr;
int fprintf(FILE *stream, const char *format, ...);
int snprintf(char *text, size_t size, const char *format, ...);
#endif
