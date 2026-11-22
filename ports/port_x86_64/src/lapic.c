/*
 * Micro OS — the local APIC of the x86-64 machine (ADR 0012).
 *
 * The local APIC is the interrupt controller of one core: the page the
 * firmware names in the MADT, the identifier of the core, the end of an
 * interrupt in service and the timer that ticks a core the 8259 cannot reach.
 * The inter-processor interrupts are the other half of the same page and they
 * live in src/ipi.c; the I/O APIC is not here yet (src/ioapic.c).
 *
 * Every register of the page is memory, not an I/O port, and every store of
 * this file is a store to the window the kernel map gives it. The window is
 * in the top 2 GiB and shared by every address space (see
 * port_x86_64_apic.h), so a core that the scheduler switched to another CR3
 * still finds its page.
 */
#include "port_x86_64_apic.h"

#include "kernel/port/cpu.h"

/* The registers of the local APIC, as offsets into its 4 KiB page (Intel
 * SDM volume 3A, 10.4.1). Only the ones this port programs are named. */
#define LAPIC_REG_ID 0x020u            /* the identifier of the core */
#define LAPIC_REG_TPR 0x080u           /* task priority */
#define LAPIC_REG_EOI 0x0B0u           /* end of interrupt */
#define LAPIC_REG_SVR 0x0F0u           /* spurious-interrupt vector */
#define LAPIC_REG_LVT_TIMER 0x320u     /* the vector of the timer */
#define LAPIC_REG_LVT_LINT0 0x350u     /* the external pin */
#define LAPIC_REG_LVT_LINT1 0x360u     /* the NMI pin */
#define LAPIC_REG_TIMER_INITIAL 0x380u /* the count to load */
#define LAPIC_REG_TIMER_CURRENT 0x390u /* the count of the moment */
#define LAPIC_REG_TIMER_DIVIDE 0x3E0u  /* the divider of the counter */

/*
 * The fields of those registers. The LVT entries are "vector in the low
 * byte, the rest is a mode": a masked entry stops the source, the periodic
 * mode reloads the counter of the timer, and the delivery mode of a pin is
 * its bits 8..10.
 */
#define LAPIC_SVR_ENABLE 0x100u
#define LAPIC_LVT_MASK 0x10000u
#define LAPIC_LVT_TIMER_PERIODIC 0x20000u
#define LAPIC_LVT_EXTINT 0x700u /* delivery 0b111: ask the 8259 for a vector */
#define LAPIC_LVT_NMI 0x400u    /* delivery 0b100: the NMI pin */

/* The divider is the value in DCR, not the number it divides by: 0b0011 is
 * sixteen, which is the slowest setting and the one a millisecond tick of a
 * kernel wants (the counter is 32 bits wide and would wrap at 1 kHz on a
 * fast machine otherwise). */
#define LAPIC_DIVIDE_16 0x3u

/*
 * The window of a 2 MiB page of the kernel map, and the count the timer
 * starts from. The count is the whole width of the register: the
 * calibration measures how far the counter moved, and a shorter count could
 * reach zero before the window closed on a slow machine.
 */
#define LAPIC_MAP_SIZE 0x200000ull
#define LAPIC_TIMER_COUNT 0xFFFFFFFFu

/* How long the calibration counts: ten milliseconds of the TSC give a
 * frequency to a hundredth of the APIC's own resolution, which is far more
 * than a tick of a millisecond needs. */
#define LAPIC_CALIBRATION_US 10000u

/* The page of the APIC in its window, NULL until the page is mapped. The
 * map and the table of the interrupts belong to the machine and are done
 * once; the registers below belong to a core and are programmed by every
 * core that calls init. */
static volatile k_u32_t *g_lapic;
static bool g_mapped;
static bool g_apic_up;

static k_u32_t lapic_read(k_u32_t reg)
{
    return g_lapic[reg / 4u];
}

static void lapic_write(k_u32_t reg, k_u32_t value)
{
    g_lapic[reg / 4u] = value;
}

/* The stubs of interrupts.S: the first instructions of the three vectors. */
extern void isr_lapic_timer(void);
extern void isr_ipi(void);
extern void isr_spurious(void);

/*
 * The page of the APIC, for the command register of an inter-processor
 * interrupt in src/ipi.c. The two halves of the work are two files and the
 * address of the window is one fact; the contract header is the contract and
 * keeps the pointer to itself, so ipi.c declares this function instead.
 */
volatile k_u32_t *port_x86_64_apic_page(void)
{
    return g_apic_up ? g_lapic : NULL;
}

bool port_x86_64_apic_init(void)
{
    k_u32_t base = port_x86_64_apic_base();

    if (base == 0u) {
        return false;
    }

    /*
     * The page of the APIC and the three entries of the table are the
     * machine's, and the first core to come here does them: every address
     * space shares the window (see the header), and every core loads the
     * same table. A second caller - the boot core of a build that asks
     * twice, or an application processor - finds both done.
     */
    if (!g_mapped) {
        /* The map takes whole 2 MiB pages of the physical range that starts
         * in the page of the device, so the chunk that holds the APIC is the
         * one that gets mapped. The offset of the device inside that chunk is
         * added to the pointer: the base of QEMU is 2 MiB aligned and the
         * offset is zero, but a firmware that names another address is still
         * answered. */
        if (port_x86_64_paging_map_device(PORT_X86_64_APIC_VIRT, base, 0x1000u) == 0u) {
            return false;
        }

        g_lapic = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_APIC_VIRT +
                                                  ((k_u64_t)base & (LAPIC_MAP_SIZE - 1u)));

        /*
         * The three vectors go into the table before the SVR may raise one.
         * The spurious vector is the reason: the store that enables the APIC
         * can be followed by a spurious interrupt, and an entry that is still
         * isr_unexpected would stop the machine on an event that carries no
         * state at all.
         */
        port_x86_64_idt_set(PORT_X86_64_VECTOR_APIC_TIMER, isr_lapic_timer);
        port_x86_64_idt_set(PORT_X86_64_VECTOR_APIC_IPI, isr_ipi);
        port_x86_64_idt_set(PORT_X86_64_VECTOR_APIC_SPURIOUS, isr_spurious);

        g_mapped = true;
    }

    /*
     * From here everything is a register of this core and cannot be
     * inherited from the core that mapped the page: the enable bit lives in
     * the spurious-vector register of each local APIC, and an application
     * processor that skipped this would take no interrupt at all - its timer
     * would count to zero in silence. The values are the same on every core,
     * so a second call of one core only writes them again.
     *
     * The two local pins keep the delivery the firmware gave them: LINT0 in
     * the external mode is how the 8259 of the port reaches the processor of
     * the boot - the timer and the keyboard arrive through it - and LINT1 is
     * the NMI. A bring-up that changed them would take the clock and the
     * console of the machine away in one store.
     */
    /*
     * The external mode belongs to the core 0 alone: the 8259 has one output
     * and the firmware wires it to the boot processor. With every core in
     * the external mode QEMU handed the timer of the 8259 to either of them
     * (measured: 1889 ticks on the core 0 and 4933 on the core 1 of 6822),
     * and the clock of the kernel, which only the core 0 advances, ran at a
     * third of real time. The other cores keep the pin masked and take their
     * ticks from their own APIC timer.
     */
    if (k_port_cpu_id() == 0u) {
        lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_EXTINT);
    } else {
        lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASK | LAPIC_LVT_EXTINT);
    }

    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_NMI);

    /* Everything this core takes is at the lowest priority: there is no
     * interrupt of the kernel that a higher task priority should delay. */
    lapic_write(LAPIC_REG_TPR, 0u);

    /*
     * Last, the enable bit and the vector of a spurious interrupt. The timer
     * is not armed here - the firmware leaves its entry as it likes and this
     * core has no clock until port_x86_64_apic_start_timer() measures the
     * counter - so a core that only wants an identifier may call this and
     * still take nothing.
     */
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | PORT_X86_64_VECTOR_APIC_SPURIOUS);

    g_apic_up = true;
    return true;
}

bool port_x86_64_apic_present(void)
{
    return g_apic_up;
}

k_u32_t port_x86_64_apic_id(void)
{
    if (!g_apic_up) {
        return 0u;
    }

    /* The identifier is one byte in the top of the register; the low bits
     * of the field are the older 82489DX shape and are not an id. */
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
}

void port_x86_64_apic_eoi(void)
{
    if (!g_apic_up) {
        return;
    }

    /* The value is not read: any store to EOI ends the interrupt that is in
     * service. */
    lapic_write(LAPIC_REG_EOI, 0u);
}

/*
 * The frequency of the counter is not a number the architecture promises:
 * the APIC counts a bus clock of the chipset, the chipset is not asked which
 * one, and QEMU's number is its own. It is therefore measured against the
 * TSC of this core, which the port calibrated against the PIT at the boot
 * and which counts at the frequency the port already knows.
 *
 * The measurement is the whole page: the counter is loaded with
 * 0xFFFFFFFF, masked, divided by sixteen, and read again after ten
 * milliseconds. The difference is the number of counts of the divider in
 * that window; a hundred windows make a second, and the initial count of a
 * tick is a second of counts divided by the ticks a second.
 *
 * The timer is masked for the whole measurement, and the caller arms it only
 * when this returns true: a machine whose PIT could not be read has no TSC
 * frequency either, and a guessed one would raise a vector that never
 * arrives. That is the honest failure of this function.
 */
bool port_x86_64_apic_start_timer(k_u32_t hz)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t span;
    k_u64_t start;
    k_u64_t frequency;
    k_u32_t remaining;
    k_u32_t counts;
    k_u32_t initial;

    if (!g_apic_up || hz == 0u || info == NULL || !info->tsc_calibrated || info->cpu_hz == 0u) {
        return false;
    }

    /* Nothing of the old programming is trusted: the entry is masked before
     * the divider and the count are written, because a timer that the
     * firmware left running would raise 0x50 with a body that is not ready
     * to take it. */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASK);
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_DIVIDE_16);
    lapic_write(LAPIC_REG_TIMER_INITIAL, LAPIC_TIMER_COUNT);

    /*
     * Ten milliseconds on the TSC of this core. The subtraction would wrap
     * at a 64-bit counter only after a century of uptime, and the comparison
     * of the two numbers as they are is what makes the wait independent of
     * the moment the counter started.
     */
    span = (info->cpu_hz * LAPIC_CALIBRATION_US) / 1000000u;
    start = port_rdtsc();

    while ((port_rdtsc() - start) < span) {
    }

    remaining = lapic_read(LAPIC_REG_TIMER_CURRENT);
    counts = LAPIC_TIMER_COUNT - remaining;

    /*
     * Two answers mean the measurement said nothing and no timer may be
     * armed: a counter that did not move (the page is not the APIC, or the
     * register did not take the write) and one that reached zero before the
     * window ended (the rate is not one this register can count). Both are
     * failures of this machine, not a rate to guess from.
     */
    if (counts == 0u || remaining == 0u) {
        return false;
    }

    frequency = ((k_u64_t)counts * 1000000u) / LAPIC_CALIBRATION_US;
    initial = (k_u32_t)(frequency / hz);

    if (initial == 0u) {
        initial = 1u;
    }

    lapic_write(LAPIC_REG_TIMER_INITIAL, initial);
    lapic_write(LAPIC_REG_LVT_TIMER, PORT_X86_64_VECTOR_APIC_TIMER | LAPIC_LVT_TIMER_PERIODIC);

    return true;
}

void port_x86_64_apic_stop_timer(void)
{
    if (!g_apic_up) {
        return;
    }

    /* The vector and the mode of the entry are cleared with the mask: a
     * stopped timer has nothing left to name. */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASK);
}

/*
 * The body of the timer of a core, called by interrupts.S with the
 * interrupts off. It is the PIT body of port_x86_64.c with the local APIC in
 * the place of the 8259: the EOI goes out first, so that the clock of this
 * core cannot stop for a task that never returns from the scheduler, and
 * then the interrupted program answers its signals and the scheduler decides
 * whose turn it is.
 */
void port_x86_64_irq_lapic_timer(k_u64_t *frame)
{
    port_x86_64_apic_eoi();
    port_x86_64_signal_tick(frame);
    port_x86_64_task_tick();
}

/*
 * A spurious interrupt is the APIC saying "an interrupt was withdrawn before
 * I could deliver it". It has no state and no EOI - a store to EOI would end
 * an interrupt of another vector that is in service - so the handler does
 * nothing and the stub returns to the code it interrupted.
 */
void port_x86_64_irq_spurious(k_u64_t *frame)
{
    (void)frame;
}
