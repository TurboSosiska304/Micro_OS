/*
 * Micro OS — the entropy of the x86-64 machine, for the TLS client.
 *
 * Two sources, in this order (ADR 0024 §3):
 *
 *   RDRAND   the instruction of the processor, when CPUID leaf 1 says the
 *            machine has it (ECX bit 30). QEMU's default model of a CPU does
 *            not, so the tests of the port usually take the second source;
 *   the timer a jitter of the TSC around a loop of the boot's own uptime and
 *            a counter, mixed with the finalizer of SplitMix64. It is not a
 *            hardware source and the file says so: a processor without
 *            RDRAND has no better one, and the handshake still needs a
 *            client random that nobody can guess.
 *
 * Both are read by mbedtls_hardware_poll() of components/net/src/tls.c, so
 * the port answers one call and the library sees a normal platform.
 */
#include "net/tls_platform.h"

#include "kernel/port/time.h"
#include "port_x86_64.h"

/* The bit of CPUID leaf 1, ECX: "RDRAND is there". */
#define CPUID_ECX_RDRAND (1u << 30)

/* The number of times one 64-bit word is asked of RDRAND before the timer
 * takes over: the instruction may answer "no" when its pool is empty. */
#define RDRAND_TRIES 10

/* The state of the fallback, mixed into every word it makes. */
static k_u64_t s_jitter;

static void cpuid(k_u32_t leaf, k_u32_t *eax, k_u32_t *ebx, k_u32_t *ecx, k_u32_t *edx)
{
    __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf), "c"(0u));
}

static bool rdrand_present(void)
{
    static int s_present = -1;
    k_u32_t eax;
    k_u32_t ebx;
    k_u32_t ecx;
    k_u32_t edx;

    if (s_present >= 0) {
        return s_present != 0;
    }

    cpuid(0u, &eax, &ebx, &ecx, &edx);
    if (eax < 1u) {
        s_present = 0;
        return false;
    }

    cpuid(1u, &eax, &ebx, &ecx, &edx);
    s_present = (ecx & CPUID_ECX_RDRAND) != 0u;

    return s_present != 0;
}

static bool rdrand_word(k_u64_t *out)
{
    k_u64_t value = 0;
    unsigned char ok = 0;

    __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));

    if (ok == 0u) {
        return false;
    }

    *out = value;

    return true;
}

/* The finalizer of SplitMix64: one multiplication is not a mix of bits. */
static k_u64_t mix64(k_u64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;

    return value ^ (value >> 31);
}

/*
 * One word of the timer. The loop between the two reads of the TSC is what
 * makes the low bits of two reads differ in a virtual machine: a timer that
 * is read twice back to back may answer the same number, and the ticks of
 * the loop are not the same length every time.
 */
static k_u64_t jitter_word(void)
{
    volatile k_u32_t spin;
    k_u64_t value;

    s_jitter = mix64(s_jitter ^ port_rdtsc() ^ ((k_u64_t)k_port_uptime_ms() << 17));

    for (spin = 0; spin < 64u; spin++) {
        s_jitter ^= port_rdtsc();
    }

    value = mix64(s_jitter + port_rdtsc());

    return value;
}

int net_tls_platform_entropy(unsigned char *out, k_size_t len)
{
    k_size_t done = 0;
    bool rdrand = rdrand_present();

    if (out == NULL) {
        return -1;
    }

    while (done < len) {
        k_u64_t word = 0;
        bool got = false;
        k_size_t take;

        if (rdrand) {
            unsigned tries;

            for (tries = 0; tries < RDRAND_TRIES && !got; tries++) {
                got = rdrand_word(&word);
            }
        }

        if (!got) {
            word = jitter_word();
        }

        take = len - done;
        if (take > sizeof(word)) {
            take = sizeof(word);
        }

        for (k_size_t i = 0; i < take; i++) {
            out[done + i] = (unsigned char)(word >> (8u * i));
        }

        done += take;
    }

    return 0;
}
