/*
 * Micro OS — the inter-processor interrupts of the x86-64 machine (ADR 0012).
 *
 * An inter-processor interrupt is one store to the command register of the
 * local APIC of another core: the destination, the vector and a delivery
 * mode. The same register starts a processor - INIT and the start-up IPI,
 * which is the sequence of the PC firmware - so the two halves that the
 * bring-up needs are here, and the vector that answers an IPI is the third.
 *
 * The page of the APIC is mapped and enabled by src/lapic.c; this file only
 * writes registers in it. Nothing here sleeps: every call is a store and a
 * wait on the TSC of the calling core, which is the only clock available
 * before a console or a scheduler exists.
 */
#include "port_x86_64_apic.h"

#include "kernel/cpu.h"
#include "kernel/port/cpu.h"

/* The command register is one 64-bit value in two 32-bit halves: the
 * destination in the high one and the command in the low one (Intel SDM
 * volume 3A, 10.6.1). The write of the low half is what sends it, so the
 * high half goes first. */
#define LAPIC_REG_ICR_LOW 0x300u
#define LAPIC_REG_ICR_HIGH 0x310u

#define LAPIC_ICR_DELIVERY_STATUS 0x1000u /* set while the command is in flight */
#define LAPIC_ICR_ASSERT 0x4000u          /* bit 14: the level is asserted */
#define LAPIC_ICR_DEASSERT 0x8000u        /* bit 15: the level is dropped again */

/* The delivery modes a bring-up sends, already in their bits 8..10. */
#define LAPIC_DELIVERY_FIXED 0x0000u
#define LAPIC_DELIVERY_INIT 0x0500u
#define LAPIC_DELIVERY_STARTUP 0x0600u

/*
 * The IPI of the scheduler has no timeout in the contract, and it needs a
 * bound all the same: a command whose delivery never completes is a bug of
 * the chipset or of the page, and a caller that spun on it forever would
 * stop the machine. Ten milliseconds is an age for one command.
 */
#define LAPIC_ICR_TIMEOUT_US 10000u

/* The page of the APIC, owned by src/lapic.c (see the comment there). */
extern volatile k_u32_t *port_x86_64_apic_page(void);

/*
 * One counter per core, in the numbering of the kernel, because that is the
 * number the shell and the tests ask with. It is written by the core that
 * takes the IPI and read by any core, so the accesses are atomic: a plain
 * read-modify-write of two cores would lose a count and the proof of the
 * path would be a proof of nothing.
 */
static k_u32_t g_ipi[KERNEL_CPUS_MAX];

static k_u32_t lapic_read(k_u32_t reg)
{
    volatile k_u32_t *page = port_x86_64_apic_page();

    return page != NULL ? page[reg / 4u] : 0u;
}

static void lapic_write(k_u32_t reg, k_u32_t value)
{
    volatile k_u32_t *page = port_x86_64_apic_page();

    if (page != NULL) {
        page[reg / 4u] = value;
    }
}

/*
 * Wait for the command register to say that it is done. The bit is set when
 * the write lands and cleared by the APIC when the destination took the
 * command; a destination that is not there leaves it set.
 *
 * The bound is the TSC, which is a frequency the port measured at the boot.
 * A machine whose PIT could not be read has no such number, and a wait that
 * cannot be bounded is not a wait: the answer is false, and the bring-up
 * reports the core it could not start.
 */
static bool icr_wait(k_u32_t timeout_us)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t span;
    k_u64_t start;

    if (info == NULL || !info->tsc_calibrated || info->cpu_hz == 0u) {
        return false;
    }

    span = ((k_u64_t)timeout_us * info->cpu_hz) / 1000000u;
    start = port_rdtsc();

    do {
        if ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS) == 0u) {
            return true;
        }
    } while ((port_rdtsc() - start) < span);

    return false;
}

/* The destination goes to the high half, the command to the low one. The
 * destination field is eight bits: an identifier that does not fit is the
 * caller's error and the low bits are still the only ones the chip has. */
static void icr_send(k_u32_t apic_id, k_u32_t command)
{
    lapic_write(LAPIC_REG_ICR_HIGH, (apic_id & 0xFFu) << 24);
    lapic_write(LAPIC_REG_ICR_LOW, command);
}

void port_x86_64_apic_send_ipi(k_u32_t apic_id, k_u32_t vector)
{
    if (!port_x86_64_apic_present()) {
        return;
    }

    /* Fixed delivery, physical destination, level asserted: the vector in
     * the low byte is the gate of the other core that runs. */
    icr_send(apic_id, (vector & 0xFFu) | LAPIC_ICR_ASSERT | LAPIC_DELIVERY_FIXED);
    (void)icr_wait(LAPIC_ICR_TIMEOUT_US);
}

bool port_x86_64_apic_send_init(k_u32_t apic_id, k_u32_t timeout_us)
{
    if (!port_x86_64_apic_present()) {
        return false;
    }

    /*
     * The level of the INIT command is asserted, then dropped: the PC
     * firmware sends both, and a chipset that sees the level stay asserted
     * would hold the destination in the reset state.
     */
    icr_send(apic_id, LAPIC_ICR_ASSERT | LAPIC_DELIVERY_INIT);
    if (!icr_wait(timeout_us)) {
        return false;
    }

    icr_send(apic_id, LAPIC_ICR_DEASSERT | LAPIC_DELIVERY_INIT);
    return icr_wait(timeout_us);
}

bool port_x86_64_apic_send_startup(k_u32_t apic_id, k_u32_t vector, k_u32_t timeout_us)
{
    if (!port_x86_64_apic_present()) {
        return false;
    }

    /* The vector names the page the core starts from: 0x07 is 0x7000. */
    icr_send(apic_id, LAPIC_ICR_ASSERT | LAPIC_DELIVERY_STARTUP | (vector & 0xFFu));
    return icr_wait(timeout_us);
}

/*
 * The body of the vector of an IPI, called by interrupts.S with the
 * interrupts off, on the stack of the code that was interrupted. The count
 * is the record of the path; the EOI goes out before the hook, because the
 * hook may switch to another task and this core must be able to take the
 * next interrupt when it comes back.
 */
void port_x86_64_irq_ipi(k_u64_t *frame)
{
    k_u32_t cpu = k_port_cpu_id();

    (void)frame;

    if (cpu < KERNEL_CPUS_MAX) {
        __atomic_add_fetch(&g_ipi[cpu], 1u, __ATOMIC_RELAXED);
    }

    port_x86_64_apic_eoi();
    port_x86_64_task_ipi();
}

k_u32_t port_x86_64_ipi_received(void)
{
    return port_x86_64_ipi_received_cpu(k_port_cpu_id());
}

k_u32_t port_x86_64_ipi_received_cpu(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return __atomic_load_n(&g_ipi[cpu], __ATOMIC_RELAXED);
}
