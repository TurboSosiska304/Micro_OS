#include "port_host.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kernel/port.h"

static FILE *s_sink;

void port_host_set_sink(FILE *sink) {
    s_sink = sink;
}

FILE *port_host_get_sink(void) {
    return s_sink != NULL ? s_sink : stdout;
}

void k_port_write(const char *data, k_size_t len) {
    if (data == NULL || len == 0) {
        return;
    }
    fwrite(data, 1, len, port_host_get_sink());
    fflush(port_host_get_sink());
}

k_u64_t k_port_uptime_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (k_u64_t)ts.tv_sec * 1000u + (k_u64_t)(ts.tv_nsec / 1000000);
}

void k_port_enter_critical(void) {
    /* Single threaded simulator: nothing to protect. */
}

void k_port_exit_critical(void) {}

void k_port_halt(void) {
    for (;;) {
        sleep(1);
    }
}

void k_port_reset(void) {
    exit(0);
}

const char *k_port_name(void) {
    return "host";
}

const char *k_port_arch(void) {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "host";
#endif
}
