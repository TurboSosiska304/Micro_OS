/*
 * Micro OS — the port of a QEMU x86-64 machine, first milestone.
 *
 * The kernel is loaded by Limine in long mode with the page tables already
 * built, so this file is plain C: the console is COM1, the clock is the
 * time stamp counter calibrated against the PIT, the memory comes from the
 * map the bootloader hands over, and a critical section is "interrupts
 * off", which on one core of this machine is exactly what it says.
 *
 * There is no task substrate yet (see port_x86_64_task.c): the first
 * milestone is that the kernel boots, checks the contract, prints its
 * banner and stops. Everything else grows from there.
 */
#include "port_x86_64.h"

#include <stdarg.h>

#include "kernel/log.h"
#include "kernel/port.h"

/* --- the machine --- */

/*
 * The low level of the machine lives in the header, where the entry code of
 * the loader can see the same instructions.
 */

/* --- the console on COM1 --- */

#define COM1 0x3F8
#define COM1_LSR 0x3FD /* line status: bit 5 "can write", bit 0 "can read" */

static void com1_init(void)
{
    port_outb(COM1 + 1, 0x00); /* no interrupts: the console is polled */
    port_outb(COM1 + 3, 0x80); /* DLAB on */
    port_outb(COM1 + 0, 0x01); /* divisor 1: 115200 baud */
    port_outb(COM1 + 1, 0x00);
    port_outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop */
    port_outb(COM1 + 2, 0xC7); /* FIFO on, cleared, 14 bytes */
    port_outb(COM1 + 4, 0x03); /* DTR and RTS */
}

void k_port_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            /* The terminal of the user wants both, the kernel writes one. */
            while ((port_inb(COM1_LSR) & 0x20) == 0) {
            }
            port_outb(COM1, '\r');
        }

        while ((port_inb(COM1_LSR) & 0x20) == 0) {
        }
        port_outb(COM1, (k_u8_t)c);
    }
}

int k_port_console_readline(char *buf, k_size_t size)
{
    k_size_t i = 0;

    if (buf == NULL || size == 0) {
        return -1;
    }

    for (;;) {
        int c = k_port_console_getchar();

        if (c < 0) {
            return -1;
        }

        if (c == '\r' || c == '\n') {
            break;
        }

        if (i + 1 < size) {
            buf[i++] = (char)c;
        }
    }

    buf[i] = '\0';
    return (int)i;
}

int k_port_console_getchar(void)
{
    if ((port_inb(COM1_LSR) & 0x01) == 0) {
        return -1;
    }

    return (int)port_inb(COM1);
}

int k_port_console_poll(void)
{
    return k_port_console_getchar();
}

/* --- the clock --- */

/*
 * The time stamp counter counts at the frequency of the processor, and the
 * frequency is not written anywhere the kernel can read. It is measured
 * once, against the programmable interval timer, which has a crystal of
 * its own: channel 2 counts down 50 ms in a one-shot mode and the counter
 * is read before and after.
 */
static k_u64_t g_tsc_per_second;
static k_u64_t g_tsc_start;

static void clock_measure(void)
{
    const k_u32_t counts = 59659; /* 50 ms of the 1193182 Hz crystal */
    k_u64_t first;
    k_u64_t last;
    k_u8_t gate;

    port_outb(0x43, 0xB0); /* channel 2, mode 0 (one-shot), low then high byte */
    port_outb(0x42, (k_u8_t)(counts & 0xFF));
    port_outb(0x42, (k_u8_t)(counts >> 8));

    /* The gate of channel 2 is bit 0 of port 0x61: low clears, high starts. */
    gate = port_inb(0x61) & (k_u8_t)~0x01;
    port_outb(0x61, gate);
    port_outb(0x61, (k_u8_t)(gate | 0x01));

    first = port_rdtsc();

    /* Bit 5 of the same port is the output of the channel: high when the
     * counter reached zero. */
    while ((port_inb(0x61) & 0x20) == 0) {
    }

    last = port_rdtsc();

    g_tsc_per_second = (last - first) * 20u;
    g_tsc_start = port_rdtsc();
}

k_u64_t k_port_uptime_us(void)
{
    if (g_tsc_per_second == 0) {
        return 0;
    }

    return (port_rdtsc() - g_tsc_start) * 1000000u / g_tsc_per_second;
}

k_u64_t k_port_uptime_ms(void)
{
    return k_port_uptime_us() / 1000u;
}

/* --- the memory --- */

/*
 * A first fit allocator over the largest usable region of the map of the
 * machine, with a header in front of every block: the block of the kernel
 * is small and the machine has megabytes, so this is the whole of the
 * manager the port needs. The region starts above the kernel image, which
 * the bootloader has already loaded.
 */
struct block {
    k_u64_t size; /* the size of the payload, the header is not counted */
    struct block *next;
    bool free;
};

static struct block *g_first;
static k_size_t g_heap_total;
static k_size_t g_heap_free;

extern char __kernel_end[];

#define BLOCK_ALIGN 16u

/* The fields of the multiboot 1 information this port reads. */
#define MB_FLAG_MMAP 0x00000040u
#define MB_MMAP_USABLE 1u

struct mb_info {
    k_u32_t flags;
    k_u32_t mem_lower;
    k_u32_t mem_upper;
    k_u32_t boot_device;
    k_u32_t cmdline;
    k_u32_t mods_count;
    k_u32_t mods_addr;
    k_u32_t syms[4];
    k_u32_t mmap_length;
    k_u32_t mmap_addr;
};

struct mb_mmap_entry {
    k_u32_t size;
    k_u32_t base_low;
    k_u32_t base_high;
    k_u32_t length_low;
    k_u32_t length_high;
    k_u32_t type;
};

static void heap_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;
    k_u64_t best_base = 0;
    k_u64_t best_size = 0;
    k_u64_t kernel_end = (k_u64_t)(uintptr_t)__kernel_end;
    k_u64_t cursor;
    k_u64_t limit;

    if (info == NULL || (info->flags & MB_FLAG_MMAP) == 0 || info->mmap_length == 0) {
        return;
    }

    cursor = info->mmap_addr;
    limit = cursor + info->mmap_length;

    while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
        const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;
        k_u64_t base = entry->base_low | ((k_u64_t)entry->base_high << 32);
        k_u64_t length = entry->length_low | ((k_u64_t)entry->length_high << 32);

        if (entry->type == MB_MMAP_USABLE && entry->base_high == 0 && length > 0) {
            if (base + length > kernel_end && length > best_size) {
                best_base = base;
                best_size = length;
            }
        }

        cursor += entry->size + sizeof(entry->size);
    }

    if (best_size == 0) {
        return;
    }

    /* The kernel sits inside the region: the heap starts after it, and the
     * region is cut to a sane size (the machine may have gigabytes, the
     * kernel needs megabytes). */
    if (best_base < kernel_end) {
        best_size -= kernel_end - best_base;
        best_base = kernel_end;
    }

    best_base = (best_base + BLOCK_ALIGN - 1) & ~(k_u64_t)(BLOCK_ALIGN - 1);

    if (best_size > 32u * 1024u * 1024u) {
        best_size = 32u * 1024u * 1024u;
    }

    if (best_size < 1024u * 1024u) {
        return;
    }

    g_first = (struct block *)(uintptr_t)best_base;
    g_first->size = best_size - sizeof(struct block);
    g_first->next = NULL;
    g_first->free = true;

    g_heap_total = (k_size_t)g_first->size;
    g_heap_free = g_heap_total;
}

void *k_port_alloc(k_size_t size, bool psram)
{
    struct block *block;

    K_UNUSED(psram); /* there is one memory on this machine */

    if (size == 0 || g_first == NULL) {
        return NULL;
    }

    size = (size + BLOCK_ALIGN - 1) & ~(k_size_t)(BLOCK_ALIGN - 1);

    for (block = g_first; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }

        /* A block that can hold the next header and a small payload is
         * split; a smaller remainder would be unusable and stays here. */
        if (block->size >= size + sizeof(struct block) + BLOCK_ALIGN) {
            struct block *rest = (struct block *)((k_u8_t *)block + sizeof(struct block) + size);

            rest->size = block->size - size - sizeof(struct block);
            rest->next = block->next;
            rest->free = true;

            block->size = size;
            block->next = rest;
        }

        block->free = false;
        g_heap_free -= block->size;

        return (k_u8_t *)block + sizeof(struct block);
    }

    return NULL;
}

void k_port_free(void *ptr)
{
    struct block *block;

    if (ptr == NULL || g_first == NULL) {
        return;
    }

    block = (struct block *)((k_u8_t *)ptr - sizeof(struct block));
    block->free = true;
    g_heap_free += block->size;

    /* The neighbours are one list, so joining is one walk from the start. */
    for (block = g_first; block != NULL && block->next != NULL; block = block->next) {
        if (block->free && block->next->free) {
            block->size += sizeof(struct block) + block->next->size;
            block->next = block->next->next;
            break;
        }
    }
}

k_size_t k_port_heap_total(void)
{
    return g_heap_total;
}

k_size_t k_port_heap_free(void)
{
    return g_heap_free;
}

k_size_t k_port_psram_total(void)
{
    return 0;
}

k_size_t k_port_psram_free(void)
{
    return 0;
}

/* --- the locks --- */

/*
 * One core, no preemption yet: a critical section is the interrupts off.
 * The depth is counted, because the kernel takes a section inside another
 * one in a few places.
 */
static k_u32_t g_critical_depth;

void k_port_enter_critical(void)
{
    port_cli();
    g_critical_depth++;
}

void k_port_exit_critical(void)
{
    if (g_critical_depth > 0) {
        g_critical_depth--;
    }

    /*
     * The interrupts stay off: this milestone has no table of the
     * interrupts, and an "sti" without one is a triple fault on the first
     * tick of the timer. The port enables them when it owns the table.
     */
}

struct k_mutex_handle {
    int unused;
};

/*
 * The mutex needs a scheduler to put a task to sleep, and the substrate of
 * the tasks does not exist yet: the milestone says so instead of pretending.
 */
k_port_mutex_t *k_port_mutex_create(void)
{
    return NULL;
}

void k_port_mutex_destroy(k_port_mutex_t *mutex)
{
    K_UNUSED(mutex);
}

int k_port_mutex_lock(k_port_mutex_t *mutex)
{
    K_UNUSED(mutex);
    return -1;
}

int k_port_mutex_unlock(k_port_mutex_t *mutex)
{
    K_UNUSED(mutex);
    return -1;
}

/* --- the ends of the machine --- */

void k_port_halt(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void k_port_reset(void)
{
    /* The controller of the keyboard resets the machine, and it is the one
     * port that every PC has had since 1984. */
    port_outb(0x64, 0xFE);
    k_port_halt();
}

/* --- the identity --- */

const char *k_port_name(void)
{
    return "x86_64";
}

const char *k_port_arch(void)
{
    return "x86_64";
}

#define PORT_API_MAJOR 1u
#define PORT_API_MINOR 2u

_Static_assert(PORT_API_MAJOR == KERNEL_PORT_API_VERSION_MAJOR,
               "port_x86_64 does not speak the contract of this kernel");

unsigned k_port_api_major(void)
{
    return PORT_API_MAJOR;
}

unsigned k_port_api_minor(void)
{
    return PORT_API_MINOR;
}

/* --- the store and the network: there are none on this milestone --- */

int k_port_kv_get(const char *key, char *buf, k_size_t size)
{
    K_UNUSED(key);
    K_UNUSED(buf);
    K_UNUSED(size);
    return -2;
}

int k_port_kv_set(const char *key, const char *value)
{
    K_UNUSED(key);
    K_UNUSED(value);
    return -1;
}

int k_port_kv_erase(const char *key)
{
    K_UNUSED(key);
    return -2;
}

int k_port_kv_commit(void)
{
    return 0;
}

int k_port_kv_next(k_size_t index, char *key, k_size_t size)
{
    K_UNUSED(index);
    K_UNUSED(key);
    K_UNUSED(size);
    return -2;
}

k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    K_UNUSED(host);
    K_UNUSED(port);
    K_UNUSED(timeout_ms);
    return NULL;
}

int k_port_net_send(k_port_socket_t *socket, const void *data, k_size_t size, bool wait)
{
    K_UNUSED(socket);
    K_UNUSED(data);
    K_UNUSED(size);
    K_UNUSED(wait);
    return -1;
}

int k_port_net_recv(k_port_socket_t *socket, void *data, k_size_t size)
{
    K_UNUSED(socket);
    K_UNUSED(data);
    K_UNUSED(size);
    return -1;
}

void k_port_net_close(k_port_socket_t *socket)
{
    K_UNUSED(socket);
}

k_port_server_t *k_port_net_listen(k_u16_t port)
{
    K_UNUSED(port);
    return NULL;
}

k_port_socket_t *k_port_net_accept(k_port_server_t *server, k_u32_t timeout_ms)
{
    K_UNUSED(server);
    K_UNUSED(timeout_ms);
    return NULL;
}

void k_port_net_server_close(k_port_server_t *server)
{
    K_UNUSED(server);
}

int k_port_net_udp_exchange(const char *host, k_u16_t port, const void *request,
                            k_size_t request_size, void *answer, k_size_t answer_size,
                            k_u32_t timeout_ms)
{
    K_UNUSED(host);
    K_UNUSED(port);
    K_UNUSED(request);
    K_UNUSED(request_size);
    K_UNUSED(answer);
    K_UNUSED(answer_size);
    K_UNUSED(timeout_ms);
    return -1;
}

/* --- the strings the compiler may ask for (there is no libc here) --- */

void *memcpy(void *dst, const void *src, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    const k_u8_t *from = (const k_u8_t *)src;
    k_size_t i;

    for (i = 0; i < size; i++) {
        to[i] = from[i];
    }

    return dst;
}

void *memset(void *dst, int value, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    k_size_t i;

    for (i = 0; i < size; i++) {
        to[i] = (k_u8_t)value;
    }

    return dst;
}

void *memmove(void *dst, const void *src, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    const k_u8_t *from = (const k_u8_t *)src;
    k_size_t i;

    if (to < from) {
        for (i = 0; i < size; i++) {
            to[i] = from[i];
        }
    } else {
        for (i = size; i > 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }

    return dst;
}

int memcmp(const void *left, const void *right, k_size_t size)
{
    const k_u8_t *a = (const k_u8_t *)left;
    const k_u8_t *b = (const k_u8_t *)right;
    k_size_t i;

    for (i = 0; i < size; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}

/* --- the init of the port --- */

static struct port_x86_64_info g_info;

void port_x86_64_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;

    com1_init();
    clock_measure();
    heap_init(multiboot_info);

    g_info.cpu_hz = g_tsc_per_second;
    g_info.usable_bytes = 0;
    g_info.boot_version = info != NULL ? 1u : 0u;

    if (info != NULL && (info->flags & MB_FLAG_MMAP) != 0) {
        k_u64_t cursor = info->mmap_addr;
        k_u64_t limit = cursor + info->mmap_length;

        while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
            const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;

            if (entry->type == MB_MMAP_USABLE) {
                g_info.usable_bytes +=
                    (k_u64_t)entry->length_low | ((k_u64_t)entry->length_high << 32);
            }

            cursor += entry->size + sizeof(entry->size);
        }
    }

    g_info.heap_total = g_heap_total;
    g_info.tsc_calibrated = g_tsc_per_second != 0;
}

const struct port_x86_64_info *port_x86_64_info(void)
{
    return &g_info;
}

void port_x86_64_panic(const char *text)
{
    k_size_t len = 0;

    if (text == NULL) {
        return;
    }

    while (text[len] != '\0') {
        len++;
    }

    k_port_write(text, len);
    k_port_halt();
}
