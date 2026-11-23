/*
 * Micro OS — the I/O APIC of the x86-64 machine (ADR 0012).
 *
 * The 8259 of a PC has one output, wired to the external pin of the local APIC
 * of the processor of the boot: every line of the machine reaches one core and
 * only one, and a machine whose firmware does not describe the legacy chip at
 * all has no clock. The I/O APIC is the other half of the interrupt
 * controller: one input per line, a redirection table, and a message to the
 * local APIC of any core the table names.
 *
 * This file programs that table. The facts it needs are the MADT's (the
 * address of the chip, the override of an ISA line), which src/cpu.c read at
 * the boot and answers through port_x86_64_ioapic_base() and its neighbours;
 * the page of the local APIC, where the end of an interrupt goes, belongs to
 * src/lapic.c. Nothing here sleeps: a register of the chip is two memory
 * accesses, and the window is mapped once by the core that gets here first.
 *
 * A machine with no I/O APIC (no ACPI, or a chip whose window reads back as
 * nothing) is not a failure of the port: port_x86_64_ioapic_init() answers
 * false, the caller keeps the 8259 of src/port_x86_64.c, and the machine has
 * the timer and the keyboard on the core of the boot, which is what every
 * version of this port before 1.6 did.
 */
#include "port_x86_64_apic.h"

#include "kernel/port/cpu.h"
#include "port_x86_64.h"
#include "port_x86_64_smp.h"

/*
 * The two registers of the window: the index of the one that is wanted and
 * the data of it (Intel 82093AA, "Register Description"). A register of the
 * chip is not memory-mapped one by one; the index selects it and the window
 * reads or writes the selected one.
 */
#define IOAPIC_REGSEL 0x00u
#define IOAPIC_WIN 0x10u

/* The registers this file programs: the identity, the version with the count
 * of the lines, and the first of the two words of a redirection entry. */
#define IOAPIC_REG_ID 0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_REG_REDIRECT 0x10u

/*
 * The fields of a redirection entry (its low word). The vector is the low
 * byte, the delivery is fixed (an ordinary interrupt of the table), the
 * destination is physical (the identifier of a local APIC and not a set of
 * them), the polarity and the trigger come from the firmware's override, and
 * the mask bit is what makes a line silent.
 */
#define IOAPIC_DELIVERY_FIXED 0x0000u
#define IOAPIC_DEST_PHYSICAL 0x0000u
#define IOAPIC_POLARITY_LOW 0x2000u
#define IOAPIC_TRIGGER_LEVEL 0x8000u
#define IOAPIC_MASK 0x10000u

/* The window of the chip in the kernel's map, beside the one of the local
 * APIC and in the same top 2 GiB that every address space shares. */
#define IOAPIC_WINDOW_SIZE 0x200000ull

/* The address of the I/O APIC of a PC when the firmware describes no table:
 * the task of the port allows the fixed address and the lines 0/1/2, which is
 * exactly what a machine of one chip has. */
#define IOAPIC_DEFAULT_PHYS 0xFEC00000u

/* The count of the lines is a byte of the version register, and the field of
 * a redirection entry is eight bits wide: 240 is the architectural ceiling. */
#define IOAPIC_ENTRIES_MAX 240u

/* The ISA lines of the port: the timer and the keyboard, in the order the
 * kernel's vectors name them. */
#define IOAPIC_LINE_TIMER 0u
#define IOAPIC_LINE_KEYBOARD 1u
#define IOAPIC_ISA_LINES 16u

/* What the port knows about one line after it programmed it. The table of the
 * chip is the truth; this copy is what the `smp irq` command prints. */
struct ioapic_line {
    k_u32_t gsi;
    k_u32_t vector;
    k_u32_t cpu;
    bool routed;
};

static volatile k_u32_t *g_ioapic;
static bool g_ioapic_up;
static k_u32_t g_ioapic_phys;
static k_u32_t g_entries;
static struct ioapic_line g_lines[IOAPIC_ISA_LINES];

static k_u32_t ioapic_read(k_u32_t reg)
{
    g_ioapic[IOAPIC_REGSEL / 4u] = reg;

    /* The index is a write of its own: reading it back is what makes the
     * window below read the register that was asked for and not the one
     * before it. */
    (void)g_ioapic[IOAPIC_REGSEL / 4u];

    return g_ioapic[IOAPIC_WIN / 4u];
}

static void ioapic_write(k_u32_t reg, k_u32_t value)
{
    g_ioapic[IOAPIC_REGSEL / 4u] = reg;
    (void)g_ioapic[IOAPIC_REGSEL / 4u];
    g_ioapic[IOAPIC_WIN / 4u] = value;
}

/*
 * One entry of the table: the vector, the core, the polarity and the trigger,
 * and whether the line is masked. The high word - the destination - goes
 * first, because the low word is the one that arms the entry.
 */
static bool entry_write(k_u32_t gsi, k_u32_t vector, k_u32_t apic_id, bool level, bool low,
                        bool masked)
{
    k_u32_t low_word;
    k_u32_t high_word;

    if (!g_ioapic_up || gsi >= g_entries) {
        return false;
    }

    low_word = (vector & 0xFFu) | IOAPIC_DELIVERY_FIXED | IOAPIC_DEST_PHYSICAL;

    if (low) {
        low_word |= IOAPIC_POLARITY_LOW;
    }

    if (level) {
        low_word |= IOAPIC_TRIGGER_LEVEL;
    }

    if (masked) {
        low_word |= IOAPIC_MASK;
    }

    high_word = (apic_id & 0xFFu) << 24;

    ioapic_write(IOAPIC_REG_REDIRECT + 2u * gsi + 1u, high_word);
    ioapic_write(IOAPIC_REG_REDIRECT + 2u * gsi, low_word);

    return true;
}

bool port_x86_64_ioapic_init(void)
{
    k_u32_t base = port_x86_64_ioapic_base();
    k_u32_t version;
    k_u32_t gsi;

    if (g_ioapic_up) {
        return true;
    }

    if (base == 0u) {
        /* The firmware described no chip. A PC has one at the fixed address
         * of the architecture, and a machine that has none reads the window
         * back as a version it cannot use - which is the false below. */
        base = IOAPIC_DEFAULT_PHYS;
    }

    if (port_x86_64_paging_map_device(PORT_X86_64_IOAPIC_VIRT, base, 0x1000u) == 0u) {
        return false;
    }

    g_ioapic = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_IOAPIC_VIRT +
                                               ((k_u64_t)base & (IOAPIC_WINDOW_SIZE - 1u)));

    /*
     * The version register says how many lines the chip has: the field is the
     * number of the last one, not the count. A window that is not the chip
     * reads back as all ones (no device) or as zeros (a page that took the
     * write and kept nothing), and both give a count the port refuses.
     */
    version = ioapic_read(IOAPIC_REG_VERSION);
    g_entries = ((version >> 16) & 0xFFu) + 1u;

    if (g_entries == 0u || g_entries > IOAPIC_ENTRIES_MAX) {
        g_ioapic = NULL;
        return false;
    }

    /*
     * Every line starts masked, with no vector and no destination: a line
     * nobody programmed must not raise a vector of the table that has no body
     * behind it. The two lines of the port are opened by the caller, one call
     * each.
     */
    for (gsi = 0; gsi < g_entries; gsi++) {
        ioapic_write(IOAPIC_REG_REDIRECT + 2u * gsi + 1u, 0u);
        ioapic_write(IOAPIC_REG_REDIRECT + 2u * gsi, IOAPIC_MASK);
    }

    g_ioapic_phys = base;
    g_ioapic_up = true;

    return true;
}

bool port_x86_64_ioapic_present(void)
{
    return g_ioapic_up;
}

k_u32_t port_x86_64_ioapic_phys(void)
{
    return g_ioapic_up ? g_ioapic_phys : 0u;
}

k_u32_t port_x86_64_ioapic_entries(void)
{
    return g_ioapic_up ? g_entries : 0u;
}

/*
 * Point the line of an ISA interrupt at a vector of a core. The line keeps
 * the polarity and the trigger the firmware gave it - the timer of the PIT is
 * edge triggered and active high on every PC, and a line an override calls
 * level triggered is one the device holds until it is served.
 *
 * The answer is false when the chip is not there or the line is past its
 * table; the caller then keeps the 8259.
 */
bool port_x86_64_ioapic_route(k_u32_t isa_irq, k_u32_t vector, k_u32_t cpu)
{
    k_u32_t gsi = port_x86_64_ioapic_gsi(isa_irq);
    k_u16_t flags = port_x86_64_ioapic_irq_flags(isa_irq);
    bool level = ((flags >> 2) & 0x3u) == 0x3u;
    bool low = (flags & 0x3u) == 0x3u;
    k_u32_t apic_id = port_x86_64_cpu_apic_id(cpu);

    if (isa_irq >= IOAPIC_ISA_LINES || !g_ioapic_up) {
        return false;
    }

    if (!entry_write(gsi, vector, apic_id, level, low, false)) {
        return false;
    }

    g_lines[isa_irq].gsi = gsi;
    g_lines[isa_irq].vector = vector;
    g_lines[isa_irq].cpu = cpu;
    g_lines[isa_irq].routed = true;

    return true;
}

/* Mask or open the line of an ISA interrupt again, without changing where it
 * goes: the entry is rewritten with the vector and the destination it has. */
bool port_x86_64_ioapic_mask(k_u32_t isa_irq, bool masked)
{
    struct ioapic_line *line;

    if (isa_irq >= IOAPIC_ISA_LINES || !g_ioapic_up) {
        return false;
    }

    line = &g_lines[isa_irq];

    if (!line->routed) {
        return false;
    }

    return entry_write(line->gsi, line->vector, port_x86_64_cpu_apic_id(line->cpu),
                       ((port_x86_64_ioapic_irq_flags(isa_irq) >> 2) & 0x3u) == 0x3u,
                       (port_x86_64_ioapic_irq_flags(isa_irq) & 0x3u) == 0x3u, masked);
}

/* --- what the `smp irq` command prints --- */

k_u32_t port_x86_64_ioapic_line_gsi(k_u32_t isa_irq)
{
    if (isa_irq >= IOAPIC_ISA_LINES || !g_lines[isa_irq].routed) {
        return 0u;
    }

    return g_lines[isa_irq].gsi;
}

k_u32_t port_x86_64_ioapic_line_vector(k_u32_t isa_irq)
{
    if (isa_irq >= IOAPIC_ISA_LINES || !g_lines[isa_irq].routed) {
        return 0u;
    }

    return g_lines[isa_irq].vector;
}

k_u32_t port_x86_64_ioapic_line_cpu(k_u32_t isa_irq)
{
    if (isa_irq >= IOAPIC_ISA_LINES || !g_lines[isa_irq].routed) {
        return 0u;
    }

    return g_lines[isa_irq].cpu;
}
