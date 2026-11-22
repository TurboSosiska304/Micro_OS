/*
 * Micro OS — the inter-processor interrupts of the x86-64 machine (ADR 0012).
 *
 * The stub of the branch: the command register of the local APIC, the INIT
 * and the start-up IPI that bring a core up, and the vector that asks a core
 * to look at its run queue. The bodies live here and in interrupts.S; until
 * they are written, the bring-up reports that it could not start a core.
 */
#include "port_x86_64_apic.h"

void port_x86_64_apic_send_ipi(k_u32_t apic_id, k_u32_t vector)
{
    (void)apic_id;
    (void)vector;
}

bool port_x86_64_apic_send_init(k_u32_t apic_id, k_u32_t timeout_us)
{
    (void)apic_id;
    (void)timeout_us;
    return false;
}

bool port_x86_64_apic_send_startup(k_u32_t apic_id, k_u32_t vector, k_u32_t timeout_us)
{
    (void)apic_id;
    (void)vector;
    (void)timeout_us;
    return false;
}

void port_x86_64_irq_ipi(k_u64_t *frame)
{
    (void)frame;
}

k_u32_t port_x86_64_ipi_received(void)
{
    return 0u;
}
