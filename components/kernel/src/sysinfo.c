/*
 * Micro OS — the platform, as the rest of the kernel sees it.
 *
 * These are the questions the upper layers used to ask the port directly.
 * The answers still come from the port, but the door is the kernel now:
 * a component that includes kernel/sysinfo.h cannot tell an ESP32 from a
 * host process, and does not need to.
 */
#include "kernel/sysinfo.h"

#include "kernel/config.h"
#include "kernel/port.h"

const char *k_sysinfo_platform_name(void)
{
    return k_port_name();
}

const char *k_sysinfo_arch(void)
{
    return k_port_arch();
}

k_size_t k_sysinfo_heap_total(void)
{
    return k_port_heap_total();
}

k_size_t k_sysinfo_heap_free(void)
{
    return k_port_heap_free();
}

k_size_t k_sysinfo_psram_total(void)
{
    /*
     * The port of the x86-64 and the null port have no external memory, but
     * they still implement the calls: the answer is zero and the kernel
     * does not ask (ADR 0010). A caller that prints the number is expected
     * to leave the line out under the same flag.
     */
#if KERNEL_PSRAM
    return k_port_psram_total();
#else
    return 0;
#endif
}

k_size_t k_sysinfo_psram_free(void)
{
#if KERNEL_PSRAM
    return k_port_psram_free();
#else
    return 0;
#endif
}

k_u64_t k_sysinfo_uptime_us(void)
{
    return k_port_uptime_us();
}

k_u64_t k_sysinfo_uptime_ms(void)
{
    /*
     * Truncation, not rounding: the microsecond clock is the one source of
     * the boot time, and a policy that waits "at least n milliseconds" must
     * not be given a moment that is still short of n.
     */
    return k_port_uptime_us() / 1000u;
}
