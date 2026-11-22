/*
 * Micro OS — the application processors of the x86-64 machine (ADR 0012).
 *
 * This file is the owner of the bring-up: the processor of the boot reads the
 * list of the firmware, gives every core a stack, writes the parameters of
 * the trampoline at 0x8000, sends INIT and the start-up IPI through the local
 * APIC, and waits for the core to answer from C. The core then stops before
 * the scheduler until kmain() releases it, because the array of the cores of
 * the kernel (kernel/cpu.c) does not exist while the port is still being
 * brought up.
 *
 * The stub below is the state of the branch while the APIC of the port is
 * being written: the boot is honest about it - one core in use, the firmware
 * list printed beside it - and nothing pretends a core that never ran.
 */
#include "port_x86_64.h"

#include "kernel/log.h"
#include "kernel/port/cpu.h"

#include "port_x86_64_apic.h"

#if KERNEL_SMP

/* The cores that reached C and answered the start-up IPI. One is the boot. */
static k_u32_t g_checked_in = 1;

void port_x86_64_smp_init(void)
{
    if (!port_x86_64_apic_init()) {
        kwarn("x86-64: no local APIC, the machine runs on the core 0");
        return;
    }

    /*
     * The AP bring-up: INIT-SIPI-SIPI for every processor of the MADT, one at
     * a time, with the integers of the trampoline written before each start.
     * It belongs to this file and is the next commit of the branch.
     */
    kwarn("x86-64: the AP bring-up is not written yet, the kernel runs on the core 0");
}

void port_x86_64_smp_release(void)
{
    /* Nothing waits before the scheduler while the stub is the bring-up. */
}

k_u32_t port_x86_64_smp_checked_in(void)
{
    return g_checked_in;
}

bool port_x86_64_smp_apic_ready(void)
{
    return port_x86_64_apic_present();
}

#else /* !KERNEL_SMP */

void port_x86_64_smp_init(void)
{
    /*
     * The one-core kernel of 1.0: the flag is the promise, not a property of
     * the machine, and a core the kernel may not use is a core not started.
     */
}

void port_x86_64_smp_release(void) {}

k_u32_t port_x86_64_smp_checked_in(void)
{
    return 1;
}

bool port_x86_64_smp_apic_ready(void)
{
    return false;
}

#endif /* KERNEL_SMP */
