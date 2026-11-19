/*
 * Micro OS — the contract of the platform port.
 *
 * Two questions are asked here.
 *
 * First, is the contract closed: does every function the kernel declares
 * have an implementation in this port? The structure below takes the
 * address of each one, with its exact signature, so a function that was
 * declared and never written fails the link of the tests - the earliest
 * moment it can be noticed on a machine that has no board.
 *
 * Second, does the port behave: a mutex of the kernel is recursive (the
 * locked helpers call each other), the clock goes forward, and the version
 * the port reports is the version the kernel speaks. The last one is the
 * vermagic of Micro OS: the boot checks it too, but a test says it in
 * plain words when the pair was built from different generations.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/port.h"

/*
 * The whole contract of the port headers in one place. The types are
 * written out on purpose: assigning a function to a member of another
 * shape is a warning of the compiler, so a signature that drifted is
 * caught here as well.
 */
struct port_contract {
    void (*write)(const char *data, k_size_t len);
    int (*console_readline)(char *buf, k_size_t size);
    int (*console_getchar)(void);
    int (*console_poll)(void);

    k_u64_t (*uptime_ms)(void);
    k_u64_t (*uptime_us)(void);

    k_size_t (*heap_free)(void);
    k_size_t (*heap_total)(void);
    k_size_t (*psram_free)(void);
    k_size_t (*psram_total)(void);
    void *(*alloc)(k_size_t size, bool psram);
    void (*free)(void *ptr);

    void (*enter_critical)(void);
    void (*exit_critical)(void);
    k_port_mutex_t *(*mutex_create)(void);
    void (*mutex_destroy)(k_port_mutex_t *mutex);
    int (*mutex_lock)(k_port_mutex_t *mutex);
    int (*mutex_unlock)(k_port_mutex_t *mutex);

    void (*halt)(void);
    void (*reset)(void);

    const char *(*name)(void);
    const char *(*arch)(void);
    unsigned (*api_major)(void);
    unsigned (*api_minor)(void);
};

static const struct port_contract s_contract = {
    .write = k_port_write,
    .console_readline = k_port_console_readline,
    .console_getchar = k_port_console_getchar,
    .console_poll = k_port_console_poll,

    .uptime_ms = k_port_uptime_ms,
    .uptime_us = k_port_uptime_us,

    .heap_free = k_port_heap_free,
    .heap_total = k_port_heap_total,
    .psram_free = k_port_psram_free,
    .psram_total = k_port_psram_total,
    .alloc = k_port_alloc,
    .free = k_port_free,

    .enter_critical = k_port_enter_critical,
    .exit_critical = k_port_exit_critical,
    .mutex_create = k_port_mutex_create,
    .mutex_destroy = k_port_mutex_destroy,
    .mutex_lock = k_port_mutex_lock,
    .mutex_unlock = k_port_mutex_unlock,

    .halt = k_port_halt,
    .reset = k_port_reset,

    .name = k_port_name,
    .arch = k_port_arch,
    .api_major = k_port_api_major,
    .api_minor = k_port_api_minor,
};

/* The number of members, so that a new subject in the header is visible
 * here: the check below names every one of them. */
static void check_contract_is_closed(void)
{
    KT_CHECK(s_contract.write != NULL);
    KT_CHECK(s_contract.console_readline != NULL);
    KT_CHECK(s_contract.console_getchar != NULL);
    KT_CHECK(s_contract.console_poll != NULL);

    KT_CHECK(s_contract.uptime_ms != NULL);
    KT_CHECK(s_contract.uptime_us != NULL);

    KT_CHECK(s_contract.heap_free != NULL);
    KT_CHECK(s_contract.heap_total != NULL);
    KT_CHECK(s_contract.psram_free != NULL);
    KT_CHECK(s_contract.psram_total != NULL);
    KT_CHECK(s_contract.alloc != NULL);
    KT_CHECK(s_contract.free != NULL);

    KT_CHECK(s_contract.enter_critical != NULL);
    KT_CHECK(s_contract.exit_critical != NULL);
    KT_CHECK(s_contract.mutex_create != NULL);
    KT_CHECK(s_contract.mutex_destroy != NULL);
    KT_CHECK(s_contract.mutex_lock != NULL);
    KT_CHECK(s_contract.mutex_unlock != NULL);

    KT_CHECK(s_contract.halt != NULL);
    KT_CHECK(s_contract.reset != NULL);

    KT_CHECK(s_contract.name != NULL);
    KT_CHECK(s_contract.arch != NULL);
    KT_CHECK(s_contract.api_major != NULL);
    KT_CHECK(s_contract.api_minor != NULL);
}

static void check_identity(void)
{
    const char *name = k_port_name();
    const char *arch = k_port_arch();

    KT_CHECK(name != NULL && strlen(name) > 0);
    KT_CHECK(arch != NULL && strlen(arch) > 0);

    KT_CHECK_INT_EQ(k_port_api_major(), KERNEL_PORT_API_VERSION_MAJOR);
    KT_CHECK_INT_EQ(k_port_api_minor(), KERNEL_PORT_API_VERSION_MINOR);
}

static void check_clock(void)
{
    k_u64_t first_ms = k_port_uptime_ms();
    k_u64_t first_us = k_port_uptime_us();
    k_u64_t second_ms = k_port_uptime_ms();

    /* The clock of the port only goes forward, and the two units agree. */
    KT_CHECK(second_ms >= first_ms);
    KT_CHECK(first_us <= second_ms * 1000u + 1000000u);
}

static void check_mutex(void)
{
    k_port_mutex_t *mutex;

    /* A NULL mutex is refused, not dereferenced. */
    KT_CHECK_INT_EQ(k_port_mutex_lock(NULL), -1);
    KT_CHECK_INT_EQ(k_port_mutex_unlock(NULL), -1);
    k_port_mutex_destroy(NULL);

    mutex = k_port_mutex_create();
    KT_CHECK(mutex != NULL);
    if (mutex == NULL) {
        return;
    }

    /* Recursive: the kernel takes the same lock from nested helpers. */
    KT_CHECK_INT_EQ(k_port_mutex_lock(mutex), 0);
    KT_CHECK_INT_EQ(k_port_mutex_lock(mutex), 0);
    KT_CHECK_INT_EQ(k_port_mutex_unlock(mutex), 0);
    KT_CHECK_INT_EQ(k_port_mutex_unlock(mutex), 0);

    k_port_mutex_destroy(mutex);
}

static void check_console(void)
{
    /* With nobody typing there is nothing to read, and the port says so
     * instead of waiting: that is the whole point of the call. */
    int c = k_port_console_poll();

    KT_CHECK(c == -1 || (c >= 0 && c <= 0xFF));
}

void test_port(void)
{
    ktest_begin("port");

    check_contract_is_closed();
    check_identity();
    check_clock();
    check_mutex();
    check_console();
}
