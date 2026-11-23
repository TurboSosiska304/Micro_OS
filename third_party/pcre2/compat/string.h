#ifndef MICRO_OS_COMPAT_STRING_H
#define MICRO_OS_COMPAT_STRING_H
#include <stddef.h>
void *memcpy(void *dest, const void *source, size_t count);
void *memmove(void *dest, const void *source, size_t count);
void *memset(void *dest, int byte, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strchr(const char *text, int letter);
void *memchr(const void *text, int letter, size_t count);
char *strrchr(const char *text, int letter);
char *strstr(const char *text, const char *needle);
#endif
