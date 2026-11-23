/*
 * Micro OS — the cores of the x86-64 machine.
 *
 * QEMU does not describe the processors in the multiboot information of the
 * port (that is a feature of multiboot 2), so the machine is asked the way a
 * PC has answered since ACPI: the root pointer in the extended BIOS area or
 * in the last kilobyte of the BIOS, the RSDT or the XSDT under it, and the
 * MADT with one entry per processor. The port reads the list, keeps the
 * hardware identifier of every processor the firmware calls enabled, and
 * remembers the address of the local APIC.
 *
 * Two numbers come out of that and they are not the same one. `available` is
 * what the firmware describes; it is the left number of the boot line and the
 * one the bring-up of src/smp.c walks. The kernel number of a core - the one
 * the scheduler, /proc and the `smp` command use - is this port's own: the
 * processor of the boot is 0 and the others follow in the order of the MADT,
 * because a core that is not started must not leave a hole in the array of
 * kernel/cpu.c whose count is the number of cores that really run.
 *
 * Which core is asking is a read of one word, not a chain of tables: every
 * core points IA32_GS_BASE at its record and k_port_cpu_id() is a `movl
 * %gs:0`. The register is loaded by port_x86_64_cpu_set_self() - the boot for
 * the core 0, an application processor as its first action in C - and a build
 * without KERNEL_SMP never touches it: there is one core and the answer is a
 * constant. FS is the thread pointer of the library and is not involved.
 */
#include "port_x86_64.h"

#include <stddef.h>
#include <string.h>

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port/cpu.h"
#include "port_x86_64_smp.h"

#define ACPI_RSDP_SIGNATURE "RSD PTR "
#define ACPI_MADT_SIGNATURE "APIC"
#define ACPI_RSDP_SEARCH_BYTES 1024u

/* The physical addresses of the two places a root pointer may live. */
#define ACPI_EBDA_POINTER 0x040Eu /* a paragraph number, 16 bytes each */
#define ACPI_BIOS_FROM 0x000E0000u
#define ACPI_BIOS_TO 0x00100000u

/* The entries of the MADT this port reads (ACPI 6.x, table 5.19). */
#define ACPI_ENTRY_LOCAL_APIC 0u
#define ACPI_ENTRY_IOAPIC 1u
#define ACPI_ENTRY_IRQ_OVERRIDE 2u
#define ACPI_ENTRY_LOCAL_X2APIC 9u

/* Bit 0 of the flags of a processor entry: it may be used. */
#define ACPI_CPU_ENABLED 0x1u

/* The ISA lines an override may rename, and the identity of a line that has
 * none: the firmware of a PC names the timer (IRQ0) as GSI 2, because the
 * cascade of the 8259 is the second input of the chip. */
#define ACPI_ISA_IRQ_MAX 16u

#define ACPI_RSDP_MIN_LENGTH 20u
#define ACPI_TABLE_HEADER 36u
#define ACPI_MADT_ENTRIES 44u

/*
 * The root pointer of ACPI (ACPI 6.x, table 5.7). The six bytes of the OEM id
 * sit between the checksum and the revision: a structure without them reads
 * the first letter of "BOCHS" as a revision of 66 and looks for an XSDT that
 * is not there (the boot found out).
 */
struct acpi_rsdp {
    char signature[8];
    k_u8_t checksum;
    char oem_id[6];
    k_u8_t revision;
    k_u32_t rsdt; /* a physical address */
    k_u32_t length;
    k_u64_t xsdt; /* a physical address, when revision >= 2 */
} __attribute__((packed));

struct acpi_header {
    char signature[4];
    k_u32_t length;
    k_u8_t revision;
    k_u8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    k_u32_t oem_revision;
    k_u32_t creator_id;
    k_u32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_header header;
    k_u32_t local_apic_address;
    k_u32_t flags;
} __attribute__((packed));

struct acpi_madt_entry {
    k_u8_t type;
    k_u8_t length;
} __attribute__((packed));

struct acpi_madt_local_apic {
    k_u8_t type;
    k_u8_t length;
    k_u8_t acpi_id;
    k_u8_t apic_id;
    k_u32_t flags;
} __attribute__((packed));

struct acpi_madt_local_x2apic {
    k_u8_t type;
    k_u8_t length;
    k_u16_t reserved;
    k_u32_t apic_id;
    k_u32_t flags;
    k_u32_t acpi_id;
} __attribute__((packed));

/* One I/O APIC of the machine: the chip that turns a line into a message to
 * one core, and the first global system interrupt it owns. A machine may have
 * several; the port keeps the first, which is the one the ISA lines reach. */
struct acpi_madt_ioapic {
    k_u8_t type;
    k_u8_t length;
    k_u8_t id;
    k_u8_t reserved;
    k_u32_t address; /* a physical address */
    k_u32_t gsi_base;
} __attribute__((packed));

/* An override of the ISA line `source`: the line reaches the I/O APIC as
 * `gsi`, with the polarity and the trigger of `flags`. Without an override a
 * line keeps its number, and is edge triggered and active high. */
struct acpi_madt_irq_override {
    k_u8_t type;
    k_u8_t length;
    k_u8_t bus;
    k_u8_t source;
    k_u32_t gsi;
    k_u16_t flags;
} __attribute__((packed));

/* The enabled processors of the MADT, in the order the firmware lists them.
 * The list is what the bring-up starts; a machine that describes more than
 * the kernel has slots for keeps only the first KERNEL_CPUS_MAX, which is the
 * same bound k_cpu_init() applies to the count. */
#define ACPI_CPU_LIST_MAX KERNEL_CPUS_MAX

static struct {
    k_u32_t available;   /* the processors the firmware calls enabled */
    k_u32_t apic_base;   /* the physical address of the local APIC */
    k_u32_t ioapic_base; /* the physical address of the first I/O APIC, 0 when none */
    k_u32_t ioapic_gsi_base;
    k_u16_t irq_gsi[ACPI_ISA_IRQ_MAX];   /* the GSI of an ISA line, the identity by default */
    k_u16_t irq_flags[ACPI_ISA_IRQ_MAX]; /* the polarity and trigger of the override */
} g_cpus;

/* One record per core, at the address IA32_GS_BASE of the core points at.
 * The kernel number is the first word on purpose: k_port_cpu_id() is a load
 * of offset zero and nothing else. The two words below it are the state the
 * fast door of a system call reads before C exists - the stack of the program
 * and the one the call keeps its own stack in (port_x86_64_smp.h names the
 * offsets, src/interrupts.S reads them). */
struct port_cpu_record {
    k_u32_t id;
    k_u32_t apic_id;
    k_u64_t kernel_rsp;
    k_u64_t user_rsp;
};

/* The offsets of the header are the offsets of the assembly: a word that
 * moved would make the entry of a call take the stack of nothing. */
_Static_assert(PORT_X86_64_CPU_ID == offsetof(struct port_cpu_record, id),
               "the kernel number is the first word of the record");
_Static_assert(PORT_X86_64_CPU_APIC_ID == offsetof(struct port_cpu_record, apic_id),
               "the hardware id follows the kernel number");
_Static_assert(PORT_X86_64_CPU_KERNEL_RSP == offsetof(struct port_cpu_record, kernel_rsp),
               "the stack of the call is at the offset the entry reads");
_Static_assert(PORT_X86_64_CPU_USER_RSP == offsetof(struct port_cpu_record, user_rsp),
               "the stack of the program is at the offset the entry reads");

static struct port_cpu_record g_records[KERNEL_CPUS_MAX];

/* The identifiers the walk of the MADT collected, and how many of them there
 * are (the count may be larger than the array: a machine of more processors
 * than the kernel has room for still says how many the firmware enabled). */
static k_u32_t g_madt_ids[ACPI_CPU_LIST_MAX];
static k_u32_t g_madt_count;

#if KERNEL_SMP
/* True once a core has pointed GS at its record. Before that - the first C of
 * the boot, and a build without SMP - the answer of k_port_cpu_id() is 0. */
static bool g_gs_ready;
#endif

static k_u8_t acpi_checksum(const void *table, k_u32_t length)
{
    const k_u8_t *bytes = (const k_u8_t *)table;
    k_u8_t sum = 0;
    k_u32_t i;

    for (i = 0; i < length; i++) {
        sum = (k_u8_t)(sum + bytes[i]);
    }

    return sum;
}

static const struct acpi_rsdp *rsdp_find_in(k_u64_t from, k_u64_t to)
{
    k_u64_t address;

    for (address = from; address + ACPI_RSDP_MIN_LENGTH <= to; address += 16u) {
        const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)(uintptr_t)address;

        if (memcmp(rsdp->signature, ACPI_RSDP_SIGNATURE, 8) != 0) {
            continue;
        }

        if (acpi_checksum(rsdp, ACPI_RSDP_MIN_LENGTH) != 0) {
            continue;
        }

        /* A revision 2 pointer covers the whole structure; an older one has
         * no length to check, so its 20 bytes are the whole of it. */
        if (rsdp->revision >= 2u && acpi_checksum(rsdp, rsdp->length) != 0) {
            continue;
        }

        return rsdp;
    }

    return NULL;
}

static const struct acpi_rsdp *rsdp_find(void)
{
    const struct acpi_rsdp *rsdp;
    k_u16_t ebda;

    /* The extended BIOS area first: that is where a machine of this size
     * keeps the tables, and the scan of a kilobyte is the rule. */
    memcpy(&ebda, (const void *)(uintptr_t)ACPI_EBDA_POINTER, sizeof(ebda));

    if (ebda != 0u) {
        k_u64_t base = (k_u64_t)ebda << 4;

        rsdp = rsdp_find_in(base, base + ACPI_RSDP_SEARCH_BYTES);

        if (rsdp != NULL) {
            return rsdp;
        }
    }

    return rsdp_find_in(ACPI_BIOS_FROM, ACPI_BIOS_TO);
}

/* Walk the tables of the root pointer and read the MADT when it is there. */
static void madt_read(const struct acpi_rsdp *rsdp)
{
    k_u64_t root;
    k_u64_t end;
    k_u64_t cursor;
    k_u64_t table_address = 0;
    k_u32_t step;

    if (rsdp->revision >= 2u && rsdp->xsdt != 0u) {
        root = rsdp->xsdt;
        step = 8u;
    } else if (rsdp->rsdt != 0u) {
        root = rsdp->rsdt;
        step = 4u;
    } else {
        return;
    }

    if (memcmp(((const struct acpi_header *)(uintptr_t)root)->signature, "XSDT", 4) != 0 &&
        memcmp(((const struct acpi_header *)(uintptr_t)root)->signature, "RSDT", 4) != 0) {
        return;
    }

    end = root + ((const struct acpi_header *)(uintptr_t)root)->length;
    cursor = root + ACPI_TABLE_HEADER;

    for (; cursor + step <= end; cursor += step) {
        k_u64_t address = (step == 8u) ? *(const k_u64_t *)(uintptr_t)cursor
                                       : (k_u64_t)(*(const k_u32_t *)(uintptr_t)cursor);
        const struct acpi_header *table = (const struct acpi_header *)(uintptr_t)address;

        if (memcmp(table->signature, ACPI_MADT_SIGNATURE, 4) == 0) {
            table_address = address;
            break;
        }
    }

    if (table_address == 0u) {
        return;
    }

    {
        const struct acpi_madt *madt = (const struct acpi_madt *)(uintptr_t)table_address;

        if (madt->header.length < sizeof(struct acpi_madt) ||
            acpi_checksum(madt, madt->header.length) != 0) {
            return;
        }

        g_cpus.apic_base = madt->local_apic_address;

        cursor = table_address + ACPI_MADT_ENTRIES;
        end = table_address + madt->header.length;

        while (cursor + sizeof(struct acpi_madt_entry) <= end) {
            const struct acpi_madt_entry *entry = (const struct acpi_madt_entry *)(uintptr_t)cursor;

            if (entry->length == 0u || cursor + entry->length > end) {
                break;
            }

            if (entry->type == ACPI_ENTRY_LOCAL_APIC &&
                entry->length >= sizeof(struct acpi_madt_local_apic)) {
                const struct acpi_madt_local_apic *cpu =
                    (const struct acpi_madt_local_apic *)(uintptr_t)cursor;

                if ((cpu->flags & ACPI_CPU_ENABLED) != 0u) {
                    g_cpus.available++;

                    if (g_madt_count < ACPI_CPU_LIST_MAX) {
                        g_madt_ids[g_madt_count] = cpu->apic_id;
                    }

                    g_madt_count++;
                }
            } else if (entry->type == ACPI_ENTRY_LOCAL_X2APIC &&
                       entry->length >= sizeof(struct acpi_madt_local_x2apic)) {
                const struct acpi_madt_local_x2apic *cpu =
                    (const struct acpi_madt_local_x2apic *)(uintptr_t)cursor;

                if ((cpu->flags & ACPI_CPU_ENABLED) != 0u) {
                    g_cpus.available++;

                    if (g_madt_count < ACPI_CPU_LIST_MAX) {
                        g_madt_ids[g_madt_count] = cpu->apic_id;
                    }

                    g_madt_count++;
                }
            } else if (entry->type == ACPI_ENTRY_IOAPIC &&
                       entry->length >= sizeof(struct acpi_madt_ioapic)) {
                const struct acpi_madt_ioapic *ioapic =
                    (const struct acpi_madt_ioapic *)(uintptr_t)cursor;

                /* The first one is the chip of the ISA lines; a machine of
                 * several keeps the others for the buses nobody programs. */
                if (g_cpus.ioapic_base == 0u && ioapic->address != 0u) {
                    g_cpus.ioapic_base = ioapic->address;
                    g_cpus.ioapic_gsi_base = ioapic->gsi_base;
                }
            } else if (entry->type == ACPI_ENTRY_IRQ_OVERRIDE &&
                       entry->length >= sizeof(struct acpi_madt_irq_override)) {
                const struct acpi_madt_irq_override *override =
                    (const struct acpi_madt_irq_override *)(uintptr_t)cursor;

                if (override->source < ACPI_ISA_IRQ_MAX) {
                    g_cpus.irq_gsi[override->source] = (k_u16_t) override->gsi;
                    g_cpus.irq_flags[override->source] = override->flags;
                }
            }

            cursor += entry->length;
        }
    }
}

/* The initial identifier of the processor that runs this code (CPUID leaf 1,
 * EBX[31:24]). It is the number the MADT names for it, which is how the boot
 * is told from the processors it starts. */
static k_u32_t cpuid_apic_id(void)
{
    k_u32_t ebx;

    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1u) : "ecx", "edx");

    return ebx >> 24;
}

void port_x86_64_cpus_init(void)
{
    const struct acpi_rsdp *rsdp;
    k_u32_t bsp;
    k_u32_t next;
    k_u32_t i;

    memset(&g_cpus, 0, sizeof(g_cpus));
    memset(g_records, 0, sizeof(g_records));
    memset(g_madt_ids, 0, sizeof(g_madt_ids));
    g_madt_count = 0;

    /* A line nobody renamed keeps its own number, and a line of the ISA bus
     * is edge triggered and active high (ACPI 6.x, 5.2.12.5). */
    for (i = 0; i < ACPI_ISA_IRQ_MAX; i++) {
        g_cpus.irq_gsi[i] = (k_u16_t)i;
    }

    rsdp = rsdp_find();

    if (rsdp != NULL) {
        madt_read(rsdp);
    }

    /*
     * A machine whose firmware said nothing is a machine of one processor:
     * counting the processor of the boot is the honest floor, and a MADT that
     * does not name it would make the kernel believe in zero cores.
     */
    if (g_cpus.available == 0u) {
        g_cpus.available = 1;
    }

    /*
     * Core 0 is the processor that runs this code, whatever its place in the
     * MADT; the others follow in the order of the table, and the entry of the
     * boot is skipped because it is already the core 0.
     */
    bsp = cpuid_apic_id();
    g_records[0].id = 0;
    g_records[0].apic_id = bsp;

    next = 1;
    for (i = 0; i < g_madt_count && i < ACPI_CPU_LIST_MAX && next < KERNEL_CPUS_MAX; i++) {
        if (g_madt_ids[i] == bsp) {
            continue;
        }

        g_records[next].id = next;
        g_records[next].apic_id = g_madt_ids[i];
        next++;
    }

    /*
     * The boot is the first core the kernel may ask which one it is, and the
     * one that has to have its record under GS before a program can make its
     * first call.
     */
    port_x86_64_cpu_set_self(0);
}

/* --- the identity of a core --- */

void port_x86_64_cpu_set_self(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        /* A core the kernel has no slot for must not open a read of the
         * array: it keeps the identity of the boot and is not counted. */
        return;
    }

    /*
     * The record is the identity of the core and the two words the entry of
     * a system call reads before there is a C frame: the stack to take and
     * the slot that keeps the stack of the program. A build of one core has
     * one record and loads it like the others - the fast door of src/
     * interrupts.S reads %gs at its first instruction, and a zero base there
     * would be a load from address zero. k_port_cpu_id() keeps answering the
     * constant zero without SMP, which is the same number by another way.
     */
    port_wrmsr(0xC0000101u, (k_u64_t)(uintptr_t)&g_records[cpu]);

#if KERNEL_SMP
    /* The flag is written after the register: a core that reads it sees the
     * base it was given, never the zero of the reset. */
    g_gs_ready = true;
#endif
}

k_u32_t port_x86_64_cpu_apic_id(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return g_records[cpu].apic_id;
}

/* --- the contract of kernel/port/cpu.h --- */

/*
 * The cores the kernel may use: the ones that reached C and were accepted by
 * the boot, not the ones the firmware lists. A machine whose second processor
 * did not answer the start-up IPI answers one here, and the boot names the
 * difference (ADR 0012).
 */
k_u32_t k_port_cpu_count(void)
{
    return port_x86_64_smp_checked_in();
}

k_u32_t k_port_cpu_id(void)
{
#if KERNEL_SMP
    k_u32_t id;

    /*
     * Before the boot has pointed GS at its record there is one core and no
     * table to read: the answer is the constant. A read of %gs:0 with a zero
     * base would be a load from address zero.
     */
    if (!g_gs_ready) {
        return 0u;
    }

    __asm__ volatile("movl %%gs:0, %0" : "=r"(id));

    return id;
#else
    /* One core: the flag is the promise, and there is no GS machinery. */
    return 0u;
#endif
}

/*
 * The stack of the program that is running, kept in the record of the core
 * for the entry of `syscall`. The caller is the one that wrote the same number
 * into RSP0 of the TSS, so the two cannot disagree: every entry into ring 3
 * goes through it (src/gdt.c).
 */
void port_x86_64_cpu_set_kernel_rsp(k_u64_t rsp)
{
    k_u32_t cpu = k_port_cpu_id();

    g_records[cpu < KERNEL_CPUS_MAX ? cpu : 0u].kernel_rsp = rsp;
}

/* --- what the boot and the shell read --- */

k_u32_t port_x86_64_cpu_available(void)
{
    return g_cpus.available;
}

k_u32_t port_x86_64_cpu_started(void)
{
    return port_x86_64_smp_checked_in();
}

k_u32_t port_x86_64_apic_base(void)
{
    return g_cpus.apic_base;
}

/* --- the I/O APIC of the firmware (the MADT, ADR 0012) --- */

k_u32_t port_x86_64_ioapic_base(void)
{
    return g_cpus.ioapic_base;
}

k_u32_t port_x86_64_ioapic_gsi(k_u32_t isa_irq)
{
    if (isa_irq >= ACPI_ISA_IRQ_MAX) {
        return isa_irq;
    }

    /* The number of the firmware is an offset from the base of the first
     * chip, which is zero on every PC the port has met. */
    return g_cpus.ioapic_gsi_base + g_cpus.irq_gsi[isa_irq];
}

k_u16_t port_x86_64_ioapic_irq_flags(k_u32_t isa_irq)
{
    if (isa_irq >= ACPI_ISA_IRQ_MAX) {
        return 0u;
    }

    return g_cpus.irq_flags[isa_irq];
}
