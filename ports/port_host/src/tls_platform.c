/*
 * Micro OS — the entropy of the host, for the TLS client.
 *
 * The simulator and the unit tests are a process of Linux, and the process
 * has an operating system under it: getrandom() is the source. The board
 * has a radio and a flash, the x86-64 machine has RDRAND or its timer, and
 * the host has this (ports/port_x86_64/src/tls_platform.c is the other
 * implementation of the same call).
 */
#include "net/tls_platform.h"

#include <sys/random.h>

int net_tls_platform_entropy(unsigned char *out, k_size_t len)
{
    k_size_t done = 0;

    if (out == NULL) {
        return -1;
    }

    while (done < len) {
        ssize_t got = getrandom(out + done, len - done, 0);

        if (got < 0) {
            return -1;
        }

        if (got == 0) {
            return -1;
        }

        done += (k_size_t)got;
    }

    return 0;
}
