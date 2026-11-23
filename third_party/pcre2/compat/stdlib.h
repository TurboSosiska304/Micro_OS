#ifndef MICRO_OS_COMPAT_STDLIB_H
#define MICRO_OS_COMPAT_STDLIB_H
#include <stddef.h>
#ifndef NULL
#define NULL ((void *)0)
#endif
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *block, size_t size);
void free(void *block);
void abort(void);
void exit(int code);
#endif
