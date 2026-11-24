/*
 * Micro OS — the port that is not a machine.
 *
 * The kernel is portable only if it needs nothing but the contract: no
 * chip, no POSIX, no file, no screen. This port is the proof of that, and
 * the build is the test: the kernel and the portable components are
 * compiled and linked against these stubs, and anything that reaches for
 * libc, a driver or a chip stops the build here instead of on a board
 * that nobody has.
 *
 * It is not a simulator: the host port (ports/port_host) is the machine
 * the tests run on. This one answers "there is nothing" and is never run.
 */
#include "kernel/port.h"
#include "kernel/port_task.h"

/* --- console --- */

void k_port_write(const char *data, k_size_t len)
{
    K_UNUSED(data);
    K_UNUSED(len);
}

void k_port_console_panic(void) {}

int k_port_console_readline(char *buf, k_size_t size)
{
    K_UNUSED(buf);
    K_UNUSED(size);
    return -1;
}

int k_port_console_getchar(void)
{
    return -1;
}

int k_port_console_poll(void)
{
    return -1;
}

/*
 * The memory a program can map. This machine has no page tables of its own
 * for a program - the board shares the memory of the kernel and the simulator
 * is a process - so every call of kernel/port/mm.h answers "no" and `mmap`
 * is ENOSYS, the same answer this port gives to `execve`.
 */
bool k_port_mm_supported(void)
{
    return false;
}

k_u64_t k_port_mm_base(void)
{
    return 0;
}

k_u64_t k_port_mm_limit(void)
{
    return 0;
}

k_u64_t k_port_mm_top(void)
{
    return 0;
}

k_u64_t k_port_mm_current(void)
{
    return 0;
}

void *k_port_mm_page(k_u64_t space, k_u64_t vaddr, k_u32_t prot)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(prot);
    return NULL;
}

void *k_port_mm_writable(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void *k_port_mm_private(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

bool k_port_mm_present(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return false;
}

void *k_port_mm_phys(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void k_port_mm_unmap(k_u64_t space, k_u64_t vaddr, k_size_t len)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(len);
}

/* --- time --- */

k_u64_t k_port_uptime_ms(void)
{
    return 0;
}

k_u64_t k_port_uptime_us(void)
{
    return 0;
}

/* --- memory --- */

k_size_t k_port_heap_free(void)
{
    return 0;
}

k_size_t k_port_heap_total(void)
{
    return 0;
}

k_size_t k_port_psram_free(void)
{
    return 0;
}

k_size_t k_port_psram_total(void)
{
    return 0;
}

void *k_port_alloc(k_size_t size, bool psram)
{
    K_UNUSED(size);
    K_UNUSED(psram);
    return NULL;
}

void k_port_free(void *ptr)
{
    K_UNUSED(ptr);
}

/* --- locking --- */

void k_port_enter_critical(void) {}

void k_port_exit_critical(void) {}

/*
 * The structure exists so that the kernel's k_mutex has somewhere to keep
 * its handle; there is no scheduler here to block a task with, so taking
 * the lock always fails and the kernel says so instead of pretending.
 */
struct k_mutex_handle {
    int unused;
};

k_port_mutex_t *k_port_mutex_create(void)
{
    return NULL;
}

int k_port_mutex_destroy(k_port_mutex_t *mutex)
{
    K_UNUSED(mutex);
    return 0;
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

/* --- power --- */

void k_port_halt(void) {}

void k_port_reset(void) {}

/* --- identity --- */

const char *k_port_name(void)
{
    return "null";
}

const char *k_port_arch(void)
{
    return "none";
}

/* The contract this port was written for. */
#define PORT_API_MAJOR 1u
#define PORT_API_MINOR 13u

_Static_assert(PORT_API_MAJOR == KERNEL_PORT_API_VERSION_MAJOR,
               "port_null does not speak the contract of this kernel");

unsigned k_port_api_major(void)
{
    return PORT_API_MAJOR;
}

unsigned k_port_api_minor(void)
{
    return PORT_API_MINOR;
}

/* --- the cores --- */

/*
 * The port that is not a machine is one core and it never runs a call: the
 * two answers exist so that the kernel links with this port too, and the
 * number is the floor of every machine (kernel/port/cpu.h).
 */
k_u32_t k_port_cpu_count(void)
{
    return 1;
}

k_u32_t k_port_cpu_id(void)
{
    return 0;
}

/* --- the store and the sockets --- */

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
