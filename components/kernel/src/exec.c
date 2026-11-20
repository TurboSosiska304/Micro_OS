/*
 * The format of a program is checked here and nowhere else: the loader of the
 * syscall layer takes the bytes of a file, asks this file whether they are an
 * image, and then asks the port to run it. Keeping the two apart means the
 * parser can be tested on a machine that cannot execute a program at all.
 */
#include "kernel/exec.h"

#include "kernel/port.h"

/* A flag this generation does not know may change the meaning of the image. */
#define K_EXEC_FLAGS_NONE 0u
#define K_EXEC_RESERVED_NONE 0u

/*
 * The header is read byte by byte and not through a struct: the bytes of a
 * file have no alignment in memory, and a cast to a k_u32_t pointer would be
 * a fault on a machine that does not forgive it (or a quiet byte swap on a
 * big-endian one).
 */
static k_u32_t exec_read_u32(const void *data, k_size_t offset)
{
    const k_u8_t *bytes = (const k_u8_t *)data + offset;

    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

int k_exec_parse(const void *data, k_size_t size, struct k_exec_image *out)
{
    k_u32_t magic;
    k_u32_t version;
    k_u32_t flags;
    k_u32_t reserved;
    k_u32_t entry;
    k_u32_t text_size;
    k_u32_t data_size;
    k_u32_t bss_size;
    k_u64_t end;
    k_u64_t entry_limit;

    if (data == NULL || out == NULL || size < K_EXEC_HEADER_SIZE) {
        return -1;
    }

    magic = exec_read_u32(data, 0);
    version = exec_read_u32(data, 4);
    flags = exec_read_u32(data, 8);
    entry = exec_read_u32(data, 12);
    text_size = exec_read_u32(data, 16);
    data_size = exec_read_u32(data, 20);
    bss_size = exec_read_u32(data, 24);
    reserved = exec_read_u32(data, 28);

    if (magic != K_EXEC_MAGIC || version != K_EXEC_VERSION) {
        return -1;
    }

    if (flags != K_EXEC_FLAGS_NONE || reserved != K_EXEC_RESERVED_NONE) {
        return -1;
    }

    /*
     * The port copies the segments word by word and clears the bss the same
     * way, so a size that is not a multiple of four would leave a tear at the
     * end of a segment. Code is mandatory: a program without a text has no
     * entry either.
     */
    if (text_size == 0 || (text_size & 3u) != 0 || (data_size & 3u) != 0 || (bss_size & 3u) != 0) {
        return -1;
    }

    if (bss_size > K_EXEC_BSS_MAX) {
        return -1;
    }

    /*
     * The sum is computed in 64 bits: the fields are 32-bit and their sum can
     * pass the end of the address space of a 32-bit port, which is exactly the
     * case a wrap-around check has to see.
     */
    end = (k_u64_t)K_EXEC_HEADER_SIZE + text_size + data_size;
    if (end > (k_u64_t)size) {
        return -1;
    }

    /*
     * The entry points into the text and nowhere else: the data may be read by
     * the program but never executed, and the header is not code at all.
     */
    entry_limit = (k_u64_t)K_EXEC_HEADER_SIZE + text_size;
    if ((k_u64_t)entry < K_EXEC_HEADER_SIZE || (k_u64_t)entry >= entry_limit) {
        return -1;
    }

    out->data = data;
    out->size = size;
    out->entry = (k_size_t)entry;
    out->text_size = (k_size_t)text_size;
    out->data_size = (k_size_t)data_size;
    out->bss_size = (k_size_t)bss_size;

    return 0;
}

int k_exec_run(const struct k_exec_image *image)
{
    void *base;
    k_u64_t entry_limit;
    k_u64_t start;
    k_u64_t end;

    if (image == NULL || image->data == NULL || image->text_size == 0) {
        return -1;
    }

    /*
     * A machine that cannot run a program says so before anything is checked
     * against its memory: the answer is the same for every image, and -2 is
     * the word the syscall layer turns into ENOSYS.
     */
    if (!k_port_exec_supported()) {
        return -2;
    }

    base = k_port_exec_base();
    if (base == NULL) {
        return -2;
    }

    /*
     * The image has to fit the window of the port. The entry has already been
     * checked by the parse, but the call may be given a structure that was not
     * parsed, so the two facts that keep the copy and the clear inside the
     * window are checked again.
     */
    entry_limit = (k_u64_t)K_EXEC_HEADER_SIZE + image->text_size;
    if ((k_u64_t)image->entry < K_EXEC_HEADER_SIZE || (k_u64_t)image->entry >= entry_limit) {
        return -1;
    }

    if (image->bss_size > K_EXEC_BSS_MAX) {
        return -1;
    }

    start = (k_u64_t)(uintptr_t)base;
    end = start + (k_u64_t)image->text_size + image->data_size + image->bss_size;

    /* A window at the zero page is not a window, and a wrapped sum is not a fit. */
    if (start == 0 || end <= start) {
        return -1;
    }

    /*
     * A program that never returns never gets here. A program that returns
     * ends the call with 0 - it has finished - and a port that refuses the
     * image answers -1.
     */
    return k_port_exec_enter(image, NULL) == 0 ? 0 : -1;
}
