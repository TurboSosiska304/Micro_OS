/*
 * Micro OS — the memory of the C library.
 *
 * Every function here is a call of the ABI of the system calls: the
 * programs of the board do not touch the memory of the kernel directly.
 */
#include "libc/stdlib.h"

#include "libc/unistd.h"

#include "kernel/errno.h"
#include "kernel/ksys.h"

void *microos_malloc(size_t size)
{
    void *block = ksys_malloc((k_size_t)size);

    if (block == NULL) {
        microos_errno();
        return NULL;
    }

    return block;
}

void *microos_realloc(void *ptr, size_t size)
{
    void *block = ksys_realloc(ptr, (k_size_t)size);

    if (block == NULL && size != 0) {
        return NULL;
    }

    return block;
}

void *microos_calloc(size_t count, size_t size)
{
    size_t total;
    void *block;

    if (count != 0 && size > (size_t)-1 / count) {
        return NULL; /* the multiplication would overflow */
    }

    total = count * size;

    block = microos_malloc(total);
    if (block == NULL) {
        return NULL;
    }

    /* The memory of the kernel is not cleared, the program must do it. */
    {
        unsigned char *bytes = (unsigned char *)block;
        size_t i;

        for (i = 0; i < total; i++) {
            bytes[i] = 0;
        }
    }

    return block;
}

void microos_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    ksys_free(ptr);
}
