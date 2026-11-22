/*
 * Micro OS — the local APIC of the x86-64 machine (ADR 0012).
 *
 * The stub of the branch: the page of the APIC and the registers of the
 * timer are the work of this file, and the inter-processor interrupts are
 * src/ipi.c. Until they are written every call answers "there is no APIC" so
 * that the bring-up reports the truth and the machine keeps the 8259 path it
 * has.
 */
#include "port_x86_64_apic.h"

bool port_x86_64_apic_init(void)
{
    return false;
}

bool port_x86_64_apic_present(void)
{
    return false;
}

k_u32_t port_x86_64_apic_id(void)
{
    return 0u;
}

void port_x86_64_apic_eoi(void) {}

bool port_x86_64_apic_start_timer(k_u32_t hz)
{
    (void)hz;
    return false;
}

void port_x86_64_apic_stop_timer(void) {}

void port_x86_64_irq_lapic_timer(k_u64_t *frame)
{
    (void)frame;
}

void port_x86_64_irq_spurious(k_u64_t *frame)
{
    (void)frame;
}
