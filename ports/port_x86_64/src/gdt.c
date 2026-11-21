/*
 * Micro OS — the descriptor tables of the x86-64 port.
 *
 * The loader has a GDT of its own, because the switch to long mode needs one
 * before there is any C to build it. It has only the two selectors the boot
 * uses, and a table written for a jump cannot grow a TSS: the kernel builds
 * the real one here, in its own memory, with the four selectors of ring 0 and
 * ring 3 and the 64-bit task state segment the processor reads when an
 * interrupt of a program crosses the privilege boundary.
 *
 * The TSS is not a task of this kernel (the scheduler keeps its own state):
 * it is the machine's place for two numbers, RSP0 and IST1. RSP0 is the stack
 * the processor takes on an interrupt from ring 3, and it belongs to the
 * program that is running, so port_exec.c writes it before every entry. IST1
 * is a stack that does not depend on any program: a double fault raised while
 * the stack of the moment is itself broken still has somewhere to land.
 */
#include "port_x86_64.h"

/* The selectors, as they sit in the table and as the processor sees them. */
#define GDT_KERNEL_CODE 0x08u
#define GDT_KERNEL_DATA 0x10u
#define GDT_USER_CODE 0x18u
#define GDT_USER_DATA 0x20u
#define GDT_TSS 0x28u

/* The two slots of a system descriptor in long mode. */
#define GDT_TSS_SLOTS 2u

#define GDT_ENTRIES (5u + GDT_TSS_SLOTS)

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

static struct gdt_entry g_gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static struct gdt_pointer g_gdt_pointer;
static struct tss64 g_tss __attribute__((aligned(16)));
static k_u8_t g_double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

/* One code or data descriptor: present, the ring in `dpl`, the flags in
 * `granularity` (G, D/B, L and AVL). */
static void gdt_set(k_u32_t index, k_u32_t dpl, k_u8_t access, k_u8_t granularity)
{
    struct gdt_entry *entry = &g_gdt[index];

    entry->limit_low = 0xFFFFu;
    entry->base_low = 0;
    entry->base_mid = 0;
    entry->access = (k_u8_t)(access | ((dpl & 3u) << 5));
    entry->granularity = granularity;
    entry->base_high = 0;
}

/* A TSS descriptor is 16 bytes and the base is split across the two slots. */
static void gdt_set_tss(k_u32_t index)
{
    k_u64_t base = (k_u64_t)(uintptr_t)&g_tss;
    k_u32_t limit = (k_u32_t)sizeof(g_tss) - 1u;
    struct gdt_entry *low = &g_gdt[index];
    struct gdt_entry *high = &g_gdt[index + 1u];

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
static void gdt_load(void)
{
    __asm__ volatile("lgdt %0" : : "m"(g_gdt_pointer));

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

void port_x86_64_gdt_init(void)
{
    /* The null descriptor is what makes selector 0 an error. */
    g_gdt[0].limit_low = 0;
    g_gdt[0].base_low = 0;
    g_gdt[0].base_mid = 0;
    g_gdt[0].access = 0;
    g_gdt[0].granularity = 0;
    g_gdt[0].base_high = 0;

    /* A 64-bit code segment and a flat data segment, ring 0. */
    gdt_set(1, 0, 0x9Au, 0xAFu);
    gdt_set(2, 0, 0x92u, 0xCFu);
    /* The same two for ring 3: what a program is allowed to run on. */
    gdt_set(3, 3, 0xFAu, 0xAFu);
    gdt_set(4, 3, 0xF2u, 0xCFu);

    /* No I/O bitmap: a pointer past the limit denies every port to ring 3. */
    g_tss.iomap = (k_u16_t)sizeof(g_tss);
    g_tss.ist[0] = (k_u64_t)(uintptr_t)(g_double_fault_stack + sizeof(g_double_fault_stack));

    gdt_set_tss(5);

    g_gdt_pointer.limit = (k_u16_t)(sizeof(g_gdt) - 1u);
    g_gdt_pointer.base = (k_u64_t)(uintptr_t)g_gdt;

    gdt_load();
}

void port_x86_64_tss_set_rsp0(k_u64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}
