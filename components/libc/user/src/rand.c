/*
 * Micro OS — the pseudo-random sequence of a program.
 *
 * The C library promises a sequence, not a generator: `rand` answers a number
 * from 0 to `RAND_MAX` and `srand` starts it at a seed. The arithmetic below
 * is the linear congruential generator of the C standard's example (the
 * multiplier 1103515245, the increment 12345 and the top fifteen bits), so a
 * program of this machine and a program of another get different numbers but
 * the same shape of the call.
 *
 * The file is separate from `src/stdlib.c` for the reason `src/strdup.c` is
 * separate from `src/string.c`: the state and the code belong to a program
 * that asks for them. bc asks for them as the fallback of its own generator
 * when the machine has no `/dev/urandom`.
 */
#include "ulibc.h"

static unsigned long g_rand_state = 1ul;

void srand(unsigned int seed)
{
    g_rand_state = (unsigned long)seed;
}

int rand(void)
{
    g_rand_state = g_rand_state * 1103515245ul + 12345ul;

    return (int)((g_rand_state >> 16) & 0x7ffful);
}
