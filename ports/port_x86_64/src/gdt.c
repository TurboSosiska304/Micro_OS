/*
 * Micro OS — the descriptor tables of the x86-64 port.
 *
 * The loader has a GDT of its own, because the switch to long mode needs one
 * before there is any C to build it. It has only the two selectors the boot
 * uses, and a table written for a jump cannot grow a TSS: the kernel builds
 * the real one here, in its own memory, with the four selectors of ring 0 and
 * ring 3, the second pair of ring 3 the SYSRET of the fast door derives its
 * numbers from, and the 64-bit task state segment the processor reads when an
 * interrupt of a program crosses the privilege boundary.
 *
 * The TSS is not a task of this kernel (the scheduler keeps its own state):
 * it is the machine's place for two numbers, RSP0 and IST1. RSP0 is the stack
 * the processor takes on an interrupt from ring 3, and it belongs to the
 * program that is running, so port_exec.c writes it before every entry. IST1
 * is a stack that does not depend on any program: a double fault raised while
 * the stack of the moment is itself broken still has somewhere to land.
 *
 * Every core has a table and a TSS of its own. RSP0 is a register of the
 * machine and the stack of a program belongs to the core that runs it, so two
 * cores cannot share one TSS: the second core would overwrite the RSP0 of the
 * first, and an interrupt of the first would land on the stack of the other
 * task. The selectors are identical in every table, which is what lets a
 * context keep its segment registers across a switch of the core.
 *
 * The IDT is not here: it is one table of the kernel, read by every core, and
 * it lives in src/port_x86_64.c with the handlers it names.
 */
#include "port_x86_64.h"

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port/console.h"
#include "kernel/port/cpu.h"
#include "port_x86_64_smp.h"

#include <stdio.h>

/* The selectors, as they sit in the table and as the processor sees them. */
#define GDT_KERNEL_CODE 0x08u
#define GDT_KERNEL_DATA 0x10u
#define GDT_USER_CODE 0x18u
#define GDT_USER_DATA 0x20u
#define GDT_TSS 0x28u

/*
 * The pair SYSRET derives its selectors from. The instruction is told one
 * number - STAR[63:48] - and takes CS from it plus 16 and SS from it plus 8;
 * the two ring 3 descriptors of the iretq frames above are in the other order,
 * so the table carries a second pair, laid out the way the instruction wants
 * it. SYSCALL is the mirror image: its CS is STAR[31:16] and its SS the
 * descriptor above, which is the kernel's own pair.
 */
#define GDT_SYSRET_DATA (PORT_X86_64_SYSRET_STAR + 0x08u)
#define GDT_SYSRET_CODE (PORT_X86_64_SYSRET_STAR + 0x10u)

/* The two slots of a system descriptor in long mode. */
#define GDT_TSS_SLOTS 2u

#define GDT_ENTRIES (5u + GDT_TSS_SLOTS + 2u)

/* A 16 KiB stack for the double fault and nothing else. */
#define DOUBLE_FAULT_STACK_SIZE (16u * 1024u)

struct gdt_entry {
    k_u16_t limit_low;
    k_u16_t base_low;
    k_u8_t base_mid;
    k_u8_t access;
    k_u8_t granularity;
    k_u8_t base_high;
} __attribute__((packed));

struct gdt_pointer {
    k_u16_t limit;
    k_u64_t base;
} __attribute__((packed));

/*
 * The 64-bit task state segment. The reserved words are part of its shape and
 * not padding to remove: the processor reads the structure by its offsets.
 */
struct tss64 {
    k_u32_t reserved0;
    k_u64_t rsp0;
    k_u64_t rsp1;
    k_u64_t rsp2;
    k_u64_t reserved1;
    k_u64_t ist[7];
    k_u64_t reserved2;
    k_u16_t reserved3;
    k_u16_t iomap;
} __attribute__((packed));

static struct gdt_entry g_gdt[KERNEL_CPUS_MAX][GDT_ENTRIES] __attribute__((aligned(16)));
static struct gdt_pointer g_gdt_pointer[KERNEL_CPUS_MAX];
static struct tss64 g_tss[KERNEL_CPUS_MAX] __attribute__((aligned(16)));
static k_u8_t g_double_fault_stack[KERNEL_CPUS_MAX][DOUBLE_FAULT_STACK_SIZE]
    __attribute__((aligned(16)));

/* The core of the table this core loads; the array has one slot without SMP. */
static k_u32_t gdt_cpu(void)
{
    k_u32_t cpu = k_port_cpu_id();

    return (cpu < KERNEL_CPUS_MAX) ? cpu : 0u;
}

/* One code or data descriptor: present, the ring in `dpl`, the flags in
 * `granularity` (G, D/B, L and AVL). */
static void gdt_set(k_u32_t cpu, k_u32_t index, k_u32_t dpl, k_u8_t access, k_u8_t granularity)
{
    struct gdt_entry *entry = &g_gdt[cpu][index];

    entry->limit_low = 0xFFFFu;
    entry->base_low = 0;
    entry->base_mid = 0;
    entry->access = (k_u8_t)(access | ((dpl & 3u) << 5));
    entry->granularity = granularity;
    entry->base_high = 0;
}

/* A TSS descriptor is 16 bytes and the base is split across the two slots. */
static void gdt_set_tss(k_u32_t cpu, k_u32_t index)
{
    k_u64_t base = (k_u64_t)(uintptr_t)&g_tss[cpu];
    k_u32_t limit = (k_u32_t)sizeof(struct tss64) - 1u;
    struct gdt_entry *low = &g_gdt[cpu][index];
    struct gdt_entry *high = &g_gdt[cpu][index + 1u];

    low->limit_low = (k_u16_t)(limit & 0xFFFFu);
    low->base_low = (k_u16_t)(base & 0xFFFFu);
    low->base_mid = (k_u8_t)((base >> 16) & 0xFFu);
    low->access = 0x89u; /* present, 64-bit available TSS */
    low->granularity = (k_u8_t)((limit >> 16) & 0x0Fu);
    low->base_high = (k_u8_t)((base >> 24) & 0xFFu);

    high->limit_low = (k_u16_t)((base >> 32) & 0xFFFFu);
    high->base_low = (k_u16_t)((base >> 48) & 0xFFFFu);
    high->base_mid = 0;
    high->access = 0;
    high->granularity = 0;
    high->base_high = 0;
}

/*
 * Load the table and reload every segment register. In long mode CS cannot be
 * written by a mov, so a far return is the one instruction that replaces it;
 * the selector and the address of the next instruction are pushed for it.
 * The thread pointer lives in an MSR and does not follow the selector of FS,
 * so it is not touched here.
 */
static void gdt_load(k_u32_t cpu)
{
    __asm__ volatile("lgdt %0" : : "m"(g_gdt_pointer[cpu]));

    __asm__ volatile("movw %w[data], %%ds\n\t"
                     "movw %w[data], %%es\n\t"
                     "movw %w[data], %%ss\n\t"
                     "xorw %%ax, %%ax\n\t"
                     "movw %%ax, %%fs\n\t"
                     "movw %%ax, %%gs\n\t"
                     "pushq %[code]\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n\t"
                     "1:\n\t"
                     :
                     : [data] "r"((k_u16_t)GDT_KERNEL_DATA), [code] "r"((k_u64_t)GDT_KERNEL_CODE)
                     : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"((k_u16_t)GDT_TSS));
}

void port_x86_64_gdt_init_cpu(k_u32_t cpu)
{
    struct gdt_entry *null;
    struct tss64 *tss;

    if (cpu >= KERNEL_CPUS_MAX) {
        return;
    }

    null = &g_gdt[cpu][0];
    tss = &g_tss[cpu];

    /* The null descriptor is what makes selector 0 an error. */
    null->limit_low = 0;
    null->base_low = 0;
    null->base_mid = 0;
    null->access = 0;
    null->granularity = 0;
    null->base_high = 0;

    /* A 64-bit code segment and a flat data segment, ring 0. */
    gdt_set(cpu, 1, 0, 0x9Au, 0xAFu);
    gdt_set(cpu, 2, 0, 0x92u, 0xCFu);
    /* The same two for ring 3: what a program is allowed to run on. */
    gdt_set(cpu, 3, 3, 0xFAu, 0xAFu);
    gdt_set(cpu, 4, 3, 0xF2u, 0xCFu);
    /* And the pair the return of the fast door needs, in its order. */
    gdt_set(cpu, GDT_SYSRET_DATA / 8u, 3, 0xF2u, 0xCFu);
    gdt_set(cpu, GDT_SYSRET_CODE / 8u, 3, 0xFAu, 0xAFu);

    /* No I/O bitmap: a pointer past the limit denies every port to ring 3. */
    tss->iomap = (k_u16_t)sizeof(struct tss64);
    tss->ist[0] =
        (k_u64_t)(uintptr_t)(g_double_fault_stack[cpu] + sizeof(g_double_fault_stack[cpu]));

    gdt_set_tss(cpu, 5);

    g_gdt_pointer[cpu].limit = (k_u16_t)(sizeof(g_gdt[cpu]) - 1u);
    g_gdt_pointer[cpu].base = (k_u64_t)(uintptr_t)g_gdt[cpu];

    gdt_load(cpu);
}

void port_x86_64_gdt_init(void)
{
    port_x86_64_gdt_init_cpu(0);
}

void port_x86_64_tss_set_rsp0(k_u64_t rsp0)
{
    g_tss[gdt_cpu()].rsp0 = rsp0;

    /*
     * The record of the core carries the same number, because the entry of
     * `syscall` has no descriptor to read on its way in: the instruction
     * changes no stack, so the stub has to find one before it can push the
     * frame, and it finds it here (src/interrupts.S). One write, one place.
     */
    port_x86_64_cpu_set_kernel_rsp(rsp0);
}

k_u64_t port_x86_64_tss_rsp0(void)
{
    return g_tss[gdt_cpu()].rsp0;
}

static const char *gdt_type_name(k_u8_t access, bool system)
{
    if (!system) {
        bool code = (access & 0x08u) != 0u;
        return code ? "code" : "data";
    }

    switch (access & 0x0Fu) {
    case 0x9u:
        return "tss64-avail";
    case 0xBu:
        return "tss64-busy";
    default:
        return "system";
    }
}

void port_x86_64_gdt_dump_entry(k_u32_t cpu, k_u32_t index)
{
    const struct gdt_entry *e;
    k_u8_t access;
    bool present;
    k_u32_t dpl;
    bool system;
    k_u64_t base;
    k_u32_t limit;
    const char *type;

    if (cpu >= KERNEL_CPUS_MAX || index >= GDT_ENTRIES) {
        kinfo("x86-64: gdt[%u] cpu %u: out of range", (unsigned)index, (unsigned)cpu);
        return;
    }

    e = &g_gdt[cpu][index];
    access = e->access;
    present = (access & 0x80u) != 0u;
    dpl = (access >> 5) & 3u;
    system = (access & 0x10u) == 0u;
    base = (k_u64_t)e->base_low | ((k_u64_t)e->base_mid << 16) | ((k_u64_t)e->base_high << 24);
    limit = (k_u32_t)e->limit_low | ((k_u32_t)(e->granularity & 0x0Fu) << 16);
    type = gdt_type_name(access, system);

    if (system && (access & 0x0Fu) >= 0x9u && (access & 0x0Fu) <= 0xBu &&
        index + 1u < GDT_ENTRIES) {
        const struct gdt_entry *h = &g_gdt[cpu][index + 1u];
        base |= ((k_u64_t)h->limit_low << 32) | ((k_u64_t)h->base_low << 48);
        kinfo("x86-64: gdt[%u] sel 0x%x %s p=%u dpl=%u base 0x%08x%08x limit 0x%x", index,
              (unsigned)(index * 8u), type, present ? 1u : 0u, dpl, (unsigned)(base >> 32),
              (unsigned)base, limit);
        return;
    }

    kinfo("x86-64: gdt[%u] sel 0x%x %s p=%u dpl=%u base 0x%x limit 0x%x flags 0x%x raw %02x %02x",
          index, (unsigned)(index * 8u), type, present ? 1u : 0u, dpl, (unsigned)base, limit,
          (unsigned)(e->granularity >> 4), (unsigned)access, (unsigned)e->granularity);
}

void port_x86_64_gdt_dump(void)
{
    k_u32_t cpu = 0u;
    k_u32_t i;

    kinfo("x86-64: GDT cpu %u base 0x%08x%08x limit 0x%x", (unsigned)cpu,
          (unsigned)(g_gdt_pointer[cpu].base >> 32), (unsigned)g_gdt_pointer[cpu].base,
          (unsigned)g_gdt_pointer[cpu].limit);

    for (i = 0; i < GDT_ENTRIES; i++) {
        const struct gdt_entry *e = &g_gdt[cpu][i];
        k_u8_t access = e->access;
        bool present = (access & 0x80u) != 0u;
        k_u32_t dpl = (access >> 5) & 3u;
        bool system = (access & 0x10u) == 0u;
        k_u64_t base =
            (k_u64_t)e->base_low | ((k_u64_t)e->base_mid << 16) | ((k_u64_t)e->base_high << 24);
        k_u32_t limit = (k_u32_t)e->limit_low | ((k_u32_t)(e->granularity & 0x0Fu) << 16);
        const char *type = gdt_type_name(access, system);

        if (system && (access & 0x0Fu) >= 0x9u && (access & 0x0Fu) <= 0xBu) {
            /* 64-bit TSS descriptor spans two slots; print the full base. */
            const struct gdt_entry *h = &g_gdt[cpu][i + 1u];
            base |= ((k_u64_t)h->limit_low << 32) | ((k_u64_t)h->base_low << 48);
            kinfo("x86-64: gdt[%u] sel 0x%x %s p=%u dpl=%u base 0x%08x%08x limit 0x%x", i,
                  (unsigned)(i * 8u), type, present ? 1u : 0u, dpl, (unsigned)(base >> 32),
                  (unsigned)base, limit);
            kinfo("x86-64: gdt[%u] sel 0x%x tss64-high (not a segment selector)", i + 1u,
                  (unsigned)((i + 1u) * 8u));
            i++; /* skip high half */
            continue;
        }

        kinfo("x86-64: gdt[%u] sel 0x%x %s p=%u dpl=%u base 0x%x limit 0x%x flags 0x%x", i,
              (unsigned)(i * 8u), type, present ? 1u : 0u, dpl, (unsigned)base, limit,
              (unsigned)(e->granularity >> 4));
    }
}
