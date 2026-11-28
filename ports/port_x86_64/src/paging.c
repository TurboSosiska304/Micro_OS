/*
 * Micro OS — the page tables of the x86-64 port.
 *
 * Two maps live here. The kernel's is the one the loader used to make: the
 * first gigabyte of the machine identity mapped in 2 MiB pages, and the image
 * itself at the top of the address space (PORT_X86_64_KERNEL_BASE), where the
 * code, the GDT, the TSS, the stacks and every static page table live. Both
 * halves are ring 0 only. Every task with a program gets a set of its own
 * whose first gigabyte is a copy of the low map and whose high half is the
 * kernel's map itself, shared, except for the window of the programs: those
 * 16 MiB are 4 KiB pages with the user bit, and the physical pages behind
 * them come from an arena outside the heap. That is what makes a program a
 * user of the machine and not a function of the kernel: the address it
 * computes is not the address the page lives at, and nothing of the kernel
 * is under it.
 *
 * A set of tables is one address space and it belongs to a task (ADR 0007,
 * K4). The tables themselves come from a pool of pages in the image; the
 * physical pages of the window come from a frame allocator over the arena.
 * A fork does not copy a byte of the parent: it copies the tables, points
 * both copies at the same frames and takes the write bit away. The fault of
 * the first write makes a private copy of that one page, which is the whole
 * of copy on write.
 *
 * Every page has a protection as well as an owner: the text of the kernel and
 * of a program is readable and executable and never writable, and every page
 * of data - the rodata, the bss, the stacks, the page tables, the arena, the
 * screen - is writable and NX. The bit that says "no code here" is bit 63 of
 * an entry, and EFER.NXE, which the loader sets, is what makes the processor
 * read it. No page of this port is writable and executable at once, and
 * port_x86_64_paging_audit() reads the tables back at the boot and before
 * every program runs to say so (ADR 0007, NX and W^X).
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/mm.h"
#include "kernel/port/cpu.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/port/lock.h"
#include "port_x86_64_task.h"

#define PAGE_SIZE 4096u
#define PAGE_SHIFT 12u
#define TABLE_ENTRIES 512u

/*
 * The two entries of the low tables that name the top 2 GiB, where the
 * kernel image is linked: PML4 entry 511 and, inside it, PDPT entry 510.
 * PORT_X86_64_KERNEL_BASE is 0xFFFFFFFF80000000, the base of that gigabyte.
 */
#define PML4_HIGH_ENTRY 511u
#define PDPT_HIGH_ENTRY 510u

#define PTE_PRESENT 0x001u
#define PTE_WRITABLE 0x002u
#define PTE_USER 0x004u
#define PTE_PAGE_SIZE 0x080u  /* in a PD entry: a 2 MiB page */
#define PTE_PAT_LARGE 0x1000u /* PAT selects the memory type of a large page */

/*
 * A page that both the parent and the child of a fork may still read and
 * neither may write: the entry of the PTE that says "copy it on the first
 * write". The bit is free for the operating system, and the processor
 * ignores it.
 */
#define PTE_COW 0x200u

/*
 * The two bits of the cache of a page: a window of a device whose bytes the
 * processor may not keep - a control register of a controller - is mapped
 * with them and the store reaches the device instead of a line of a cache
 * (port_x86_64_paging_map_mmio). A framebuffer is memory rather than a register
 * and uses PAT write-combining below.
 */
#define PTE_PCD 0x010u
#define PTE_PWT 0x008u
#define PTE_CACHE_MASK (PTE_PCD | PTE_PWT | PTE_PAT_LARGE)

/* IA32_PAT has eight memory-type fields of eight bits (the low three name the
 * type). Entry 5 is the write-combining entry selected by PAT=1, PCD=0 and
 * PWT=1. The port writes and reads it back instead of assuming that a laptop
 * left the register alone. */
#define IA32_PAT_MSR 0x277u
#define IA32_MTRR_DEF_TYPE_MSR 0x2FFu
#define PAT_WC_ENTRY 5u
#define PAT_TYPE_MASK 0xFFu
#define PAT_TYPE_WC 0x01u

/*
 * The no-execute bit, the highest of the entry. It only means anything when
 * EFER.NXE is set, which the loader does before it turns paging on (see
 * loader32.S); without it bit 63 is a reserved bit and a page fault. Every
 * page that holds data, a stack, a page table or a device is marked with it,
 * and only the text of the kernel and of a program is left executable
 * (ADR 0007, NX and W^X).
 */
#define PTE_NX ((k_u64_t)1u << 63)

#define PTE_ADDRESS 0x000FFFFFFFFFF000ull

/*
 * The two bits of CR4 that keep the kernel out of the memory of a program
 * (ADR 0052). SMEP refuses a fetch of a page of ring 3 from ring 0 and SMAP
 * refuses a read or a write of one unless AC of RFLAGS is set. The bits are
 * the right boundary for a kernel whose address space is separate from the
 * program's, which is the promise of ADR 0052.
 *
 * This port does not keep that promise yet on real metal: the kernel enters
 * a program on tables that still share the low map with it, and on a Ryzen
 * machine the first program took a general protection fault from these bits
 * before it printed a byte. The boot therefore clears both bits on every
 * core and says the CR4 it read and the one it wrote; the supervisor bit of
 * the pages and the checks of the copy edge are the boundary until there is
 * a separate map for the kernel (the debt of docs/debts.md, ADR 0052). A
 * processor that does not advertise either bit loses nothing.
 */
#define CR4_SMEP ((k_u64_t)1u << 20)
#define CR4_SMAP ((k_u64_t)1u << 21)

/* What the boot found and left on: always false while the policy above
 * holds. A core of a many-core machine probes the same bits and writes the
 * same number, so the pair is a fact about the processor, not a race. */
static bool g_smep_on;
static bool g_smap_on;

/* The memory-type registers are per core, like CR4. Every core writes the same
 * reserved PAT entry and verifies it before it enters the scheduler. A CPU
 * without PAT keeps the framebuffer write-through (PCD=0, PWT=1), which is
 * ordinary cacheable memory and still reaches the device on every write. */
static bool g_pat_wc_ready;
static k_u64_t g_pat_value;
static bool g_mtrr_on;

/* The 2 MiB a single PD entry covers, and the pages under a PTE table. */
#define LARGE_PAGE_SIZE 0x200000u

#define USER_PD_FIRST ((PORT_X86_64_USER_BASE >> 21) & (TABLE_ENTRIES - 1u))
#define USER_PD_COUNT ((PORT_X86_64_USER_LIMIT - PORT_X86_64_USER_BASE) >> 21)
#define USER_FRAME_COUNT ((PORT_X86_64_USER_LIMIT - PORT_X86_64_USER_BASE) / PAGE_SIZE)

/*
 * The page tables of the spaces. A space needs twelve pages - a PML4, a
 * PDPT, a PD, a PTE table for the low memory and eight for the window - so
 * the pool holds enough for a score of programs and their children before an
 * exec starts saying "no memory". It is in the image and not in the heap
 * because the pool has to outlive the allocator's bookkeeping and be page
 * aligned by the linker, not by a request.
 */
#define PAGE_POOL_PAGES 512u
#define PAGE_POOL_WORDS (PAGE_POOL_PAGES / 64u)

/* True once the port has replaced the tables of the loader with its own. The
 * map of the loader covers the whole memory of the machine (a machine of UEFI
 * reads its ACPI tables above the first gigabyte there); the map of the port
 * covers the first gigabyte and the windows it makes. A reader of the firmware
 * tables (src/cpu.c) asks this to know which of the two maps is the one that
 * runs. */
static bool g_paging_ready;

static k_u64_t g_kernel_pml4[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pdpt_low[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pd_low[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pdpt_high[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pd_high[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

/*
 * The pages of the kernel image are not uniform: the text is executable and
 * read-only, the rodata is read-only and NX, and everything after it - data,
 * bss, the stacks and the tables - is writable and NX. A 2 MiB page cannot
 * carry three protections, so the chunks of the map that the image sits in
 * are split into 4 KiB pages with a table of their own. The pool is in the
 * image and sized for a kernel of 16 MiB, which is far more than the image of
 * the port and still bounded.
 *
 * One table serves both names of a chunk: an entry holds the physical
 * address, so the low identity map and the map at the top differ only in the
 * index of the PD entry that names the table. The text is therefore read-only
 * and the data NX through either address, which is what keeps W^X true for
 * the kernel itself.
 */
#define KERNEL_IMAGE_TABLES 8u
static k_u64_t g_kernel_image_pt[KERNEL_IMAGE_TABLES][TABLE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));

/*
 * The first 2 MiB get a table of 4 KiB pages for one reason: the AP
 * trampoline at 0x7000 (src/ap_trampoline.S) is code the second processor
 * executes after it turns the paging on, and a 2 MiB page cannot be
 * executable and hold the writable parameter block at 0x8000 at once. Page 7
 * of the low map - 0x7000..0x7FFF - is therefore present and executable and
 * read-only, the parameter page 8 is writable and NX like the rest of the
 * data, and the boot's W^X audit still finds no page that is both writable
 * and executable.
 */
static k_u64_t g_kernel_low_pt[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

/* The page of the AP trampoline; the page of its parameters (8) keeps the
 * writable NX protection of the data it is. */
#define AP_TRAMPOLINE_PAGE 7u

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __kernel_end[];

static k_u8_t g_page_pool[PAGE_POOL_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_page_bitmap[PAGE_POOL_WORDS];

/*
 * Frame lifecycle diagnostics for the SMP memory-corruption hunt. Set to 0
 * to keep the console quiet on a normal run; set to 1 for one instrumented
 * boot. Every log is below this flag, and the reverse-map invariant runs only
 * when it is on.
 */
#define PORT_FRAME_DIAG 0
/* The per-frame journal is far too verbose for a live console: every alloc
 * and free lands between a shell command and its answer. Keep it off in a
 * normal run; only invariant violations print, which are rare. */
#define PORT_FRAME_TRACE 0

/*
 * One reference per page of the arena. A fork makes two spaces point at the
 * same frame, so a space that ends may not free a page the other one still
 * reads; the count is what tells the last one from the first one.
 */
static k_u16_t g_frame_ref[USER_FRAME_COUNT];

/*
 * The address a page-table entry has to carry for a page of the image. The
 * kernel is linked in the higher half and the loader puts the bytes at
 * KERNEL_PHYS, so a pointer of the image is not a physical address any more
 * (ADR 0007). A pointer of the heap still is, because the heap is the low
 * identity map.
 */
static k_u64_t page_phys(const void *page)
{
    return port_x86_64_kernel_phys(page);
}

/*
 * The pool page behind either of the two names of it. A page that comes back
 * from a walk of the tables is a physical address, because that is what an
 * entry holds; the pool hands its pages out by their high address. The free
 * list has one entry per page, so the two names have to meet here.
 */
static k_u8_t *pool_page(k_u8_t *page)
{
    k_u64_t physical = (k_u64_t)(uintptr_t)page;
    k_u64_t pool_phys = page_phys(g_page_pool);

    if (physical < pool_phys || physical >= pool_phys + sizeof(g_page_pool)) {
        return page;
    }

    return g_page_pool + (physical - pool_phys);
}

/*
 * The pool of the page tables and the frames of the arena are one state of
 * the machine, not one per core: two cores that fault at the same moment
 * would take the same free page, and the two spaces would share the table or
 * the data of each other. The sections are short, they never sleep, and they
 * use the lock of the machine (the big kernel lock of the port), which is
 * recursive and therefore safe from a fault that interrupted a section the
 * same core already holds.
 */
static void *page_pool_take(bool zero);

static void *page_pool_alloc(void)
{
    return page_pool_take(true);
}

static void *page_pool_take(bool zero)
{
    k_u32_t word;
    void *page = NULL;

    k_port_enter_critical();

    for (word = 0; word < PAGE_POOL_WORDS && page == NULL; word++) {
        k_u32_t bit;

        if (g_page_bitmap[word] == ~(k_u64_t)0) {
            continue;
        }

        for (bit = 0; bit < 64u; bit++) {
            k_u64_t mask = (k_u64_t)1u << bit;

            if ((g_page_bitmap[word] & mask) == 0) {
                k_u8_t *found;

                g_page_bitmap[word] |= mask;
                found = g_page_pool + ((k_size_t)word * 64u + bit) * PAGE_SIZE;
                page = found;
                break;
            }
        }
    }

    k_port_exit_critical();

    if (page != NULL && zero) {
        memset(page, 0, PAGE_SIZE);
    }

    return page;
}

static void page_pool_free(void *page)
{
    k_u8_t *base = pool_page((k_u8_t *)page);
    k_size_t index;
    k_u32_t word;
    k_u32_t bit;

    if (base < g_page_pool || base >= g_page_pool + sizeof(g_page_pool)) {
        return;
    }

    index = (k_size_t)(base - g_page_pool) / PAGE_SIZE;
    word = (k_u32_t)(index / 64u);
    bit = (k_u32_t)(index % 64u);

    k_port_enter_critical();
    g_page_bitmap[word] &= ~((k_u64_t)1u << bit);
    k_port_exit_critical();
}

/* --- the frames of the physical arena ------------------------------------- */

/*
 * A page of the arena is taken by the first space that maps it and given back
 * by the last one: the count is the number of spaces that name the frame, and
 * a fork is what makes it two. `g_frame_hint` is where the allocator starts
 * looking: a frame that is free is usually the one after the last one taken,
 * and the linear scan of a 96 MiB arena (24576 frames) would be the price of
 * every fault. The release pulls the hint back to the frame it gave up.
 */
static k_u32_t g_frame_hint;

static inline __attribute__((always_inline)) bool frame_index(k_u64_t address, k_size_t *index)
{
    if (address < PORT_X86_64_USER_PHYS ||
        address >= PORT_X86_64_USER_PHYS + (k_u64_t)USER_FRAME_COUNT * PAGE_SIZE) {
        return false;
    }

    *index = (k_size_t)(address - PORT_X86_64_USER_PHYS) / PAGE_SIZE;

    return true;
}

/* The two halves a caller that holds the critical section already needs:
 * a fork shares a program of 64 MiB with one lock and not sixteen thousand,
 * and the release of a space is the same walk in the other direction. */
static inline __attribute__((always_inline)) bool frame_ref_locked(k_u64_t address)
{
    k_size_t index;

    if (!frame_index(address, &index)) {
        return false;
    }

    if (g_frame_ref[index] == 0xFFFFu) {
        return false;
    }

    g_frame_ref[index]++;
    return true;
}

static inline __attribute__((always_inline)) void frame_release_locked(k_u64_t address)
{
    k_size_t index;

    if (!frame_index(address, &index)) {
        return;
    }

    if (g_frame_ref[index] == 0) {
        return;
    }

    g_frame_ref[index]--;

    if (g_frame_ref[index] == 0 && index < g_frame_hint) {
        g_frame_hint = (k_u32_t)index;
    }
}

static k_u64_t *table_entry(k_u64_t entry);
static k_u64_t *space_pd(k_u64_t cr3);

#if PORT_FRAME_DIAG
/*
 * Reverse map: find an active address space whose page tables still name a
 * frame. Called before a zero-refcount frame is handed out; a hit means a
 * space kept a PTE to a frame the allocator believes is free.
 */
static bool frame_scan_users(k_u64_t frame, k_u32_t *out_pid, k_u64_t *out_cr3)
{
    k_size_t i;

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index(i);
        k_u64_t cr3;
        k_u64_t *pd;
        k_u32_t pd_index;

        if (t == NULL) {
            continue;
        }
        cr3 = port_x86_64_task_cr3_of(t);
        if (cr3 == 0 || cr3 == port_x86_64_kernel_cr3()) {
            continue;
        }
        pd = space_pd(cr3);
        if (pd == NULL) {
            continue;
        }
        for (pd_index = 0; pd_index < USER_PD_COUNT; pd_index++) {
            k_u64_t pde = pd[USER_PD_FIRST + pd_index];
            k_u64_t *pt;
            k_u32_t pt_index;

            if ((pde & PTE_PRESENT) == 0 || (pde & PTE_PAGE_SIZE) != 0) {
                continue;
            }
            pt = table_entry(pde);
            for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
                if ((pt[pt_index] & PTE_PRESENT) != 0 && (pt[pt_index] & PTE_ADDRESS) == frame) {
                    if (out_pid != NULL) {
                        *out_pid = t->pid;
                    }
                    if (out_cr3 != NULL) {
                        *out_cr3 = cr3;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}
#endif

k_u64_t port_x86_64_frame_alloc(void)
{
    k_size_t index;
    k_size_t slot;
    k_u64_t address = 0;
#if PORT_FRAME_DIAG
    k_u32_t holder_pid = 0;
    k_u64_t holder_cr3 = 0;
    bool held_by_space = false;
#endif

    k_port_enter_critical();

    for (slot = 0; slot < USER_FRAME_COUNT; slot++) {
        index = (k_size_t)g_frame_hint + slot;

        if (index >= USER_FRAME_COUNT) {
            index -= USER_FRAME_COUNT;
        }

        if (g_frame_ref[index] == 0) {
#if PORT_FRAME_DIAG
            address = PORT_X86_64_USER_PHYS + (k_u64_t)index * PAGE_SIZE;
            held_by_space = frame_scan_users(address, &holder_pid, &holder_cr3);
#endif
            g_frame_ref[index] = 1;
            g_frame_hint = (k_u32_t)((index + 1u) % USER_FRAME_COUNT);
            address = PORT_X86_64_USER_PHYS + (k_u64_t)index * PAGE_SIZE;
            break;
        }
    }

#if PORT_FRAME_DIAG
    if (held_by_space) {
        klog(K_LOG_LEVEL_ERROR,
             "frame-invariant: ALLOC of %08x%08x still mapped by pid %u cr3 %08x%08x (refs now 1)",
             (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu), (unsigned)holder_pid,
             (unsigned)(holder_cr3 >> 32), (unsigned)(holder_cr3 & 0xFFFFFFFFu));
    }
#endif
    k_port_exit_critical();

#if PORT_FRAME_TRACE
    if (address != 0) {
        struct task *self = task_self();
        klog(K_LOG_LEVEL_ERROR, "frame-alloc: %08x%08x pid=%u cpu=%u cr3=%08x%08x",
             (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu),
             self != NULL ? (unsigned)self->pid : 0u, (unsigned)k_port_cpu_id(),
             (unsigned)(port_x86_64_current_cr3() >> 32),
             (unsigned)(port_x86_64_current_cr3() & 0xFFFFFFFFu));
    }
#endif

    if (address != 0) {
        /* A new page of a program reads as zeros, not as the stack of the
         * program that had the frame before it. */
        memset((void *)(uintptr_t)address, 0, PAGE_SIZE);
    }

    return address;
}

void port_x86_64_frame_release(k_u64_t address)
{
#if PORT_FRAME_DIAG
    k_size_t index;
    k_u16_t before;
#endif

    k_port_enter_critical();
#if PORT_FRAME_DIAG
    if (frame_index(address, &index)) {
        before = g_frame_ref[index];
        if (before == 0) {
            struct task *self = task_self();
            klog(K_LOG_LEVEL_ERROR,
                 "frame-invariant: RELEASE of %08x%08x with refs 0 pid=%u cpu=%u cr3=%08x%08x",
                 (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu),
                 self != NULL ? (unsigned)self->pid : 0u, (unsigned)k_port_cpu_id(),
                 (unsigned)(port_x86_64_current_cr3() >> 32),
                 (unsigned)(port_x86_64_current_cr3() & 0xFFFFFFFFu));
        }
    }
#endif
    frame_release_locked(address);
    k_port_exit_critical();

#if PORT_FRAME_TRACE
    if (frame_index(address, &index)) {
        struct task *self = task_self();
        klog(K_LOG_LEVEL_ERROR, "frame-free: %08x%08x refs=%u pid=%u cpu=%u cr3=%08x%08x",
             (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu),
             (unsigned)g_frame_ref[index], self != NULL ? (unsigned)self->pid : 0u,
             (unsigned)k_port_cpu_id(), (unsigned)(port_x86_64_current_cr3() >> 32),
             (unsigned)(port_x86_64_current_cr3() & 0xFFFFFFFFu));
    }
#endif
}

bool port_x86_64_frame_hold(k_u64_t address)
{
    bool held;

    k_port_enter_critical();
    held = frame_ref_locked(address);
    k_port_exit_critical();

    return held;
}

/* --- the tables of one address space -------------------------------------- */

/*
 * A table of a walk as the processor names it: the entry holds a physical
 * address, and the port reads the table through the low identity map, which
 * is a name of every frame of the image. The tables of the address spaces
 * and the kernel's own are all in the image; a caller that keeps the high
 * name of a page (page_pool_alloc) and one that keeps the low one (this
 * function) reach the same bytes.
 */
static k_u64_t *table_entry(k_u64_t entry)
{
    return (k_u64_t *)(uintptr_t)(entry & PTE_ADDRESS);
}

static k_u64_t *space_pd(k_u64_t cr3)
{
    k_u64_t *pml4 = (k_u64_t *)(uintptr_t)cr3;

    if (pml4 == NULL || (pml4[0] & PTE_PRESENT) == 0) {
        return NULL;
    }

    return table_entry(table_entry(pml4[0])[0]);
}

static k_u64_t *space_pt(k_u64_t cr3, k_u64_t pd_index)
{
    k_u64_t *pd = space_pd(cr3);

    if (pd == NULL || pd_index >= TABLE_ENTRIES || (pd[pd_index] & PTE_PRESENT) == 0 ||
        (pd[pd_index] & PTE_PAGE_SIZE) != 0) {
        return NULL;
    }

    return table_entry(pd[pd_index]);
}

static void write_cr3(k_u64_t cr3)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

static void invlpg(k_u64_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static k_u8_t pat_entry(k_u64_t value, k_u32_t index)
{
    return (k_u8_t)((value >> (index * 8u)) & PAT_TYPE_MASK);
}

static k_u64_t pat_set_entry(k_u64_t value, k_u32_t index, k_u8_t type)
{
    k_u64_t shift = (k_u64_t)index * 8u;

    return (value & ~((k_u64_t)PAT_TYPE_MASK << shift)) | ((k_u64_t)type << shift);
}

/*
 * A window of a device in the map of the kernel: 2 MiB pages of the top 2 GiB
 * that name the physical range of the device, writable and NX. The windows of
 * the APIC, the IOAPIC and ACPI use this cacheable map; the framebuffer has its
 * own memory type below. Every address space shares the PDPT of the top 2 GiB,
 * so a store of a system call reaches the device whichever space the processor
 * is on.
 *
 * Both addresses are taken to whole 2 MiB pages, and the answer is the number
 * of pages that were mapped (zero when the range is not in the window of the
 * table or the arguments are not aligned). The caller adds the offset of the
 * device inside its first page to the virtual address.
 */
k_u32_t port_x86_64_paging_map_device(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    const k_u64_t high_end = PORT_X86_64_KERNEL_BASE + 0x40000000ull;
    k_u64_t first = paddr & ~(k_u64_t)(LARGE_PAGE_SIZE - 1u);
    k_u64_t last = paddr + (k_u64_t)len;
    k_u64_t chunk;
    k_u32_t index = (k_u32_t)((vaddr >> 21) & (TABLE_ENTRIES - 1u));
    k_u32_t pages = 0;

    if (len == 0 || (vaddr & (k_u64_t)(LARGE_PAGE_SIZE - 1u)) != 0 ||
        vaddr < PORT_X86_64_KERNEL_BASE) {
        return 0;
    }

    if (vaddr + (last - first) > high_end) {
        return 0;
    }

    for (chunk = first; chunk < last; chunk += LARGE_PAGE_SIZE) {
        if (index >= TABLE_ENTRIES) {
            return 0;
        }

        g_kernel_pd_high[index] =
            (chunk & PTE_ADDRESS) | PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE | PTE_NX;
        index++;
        pages++;
    }

    return pages;
}

/*
 * The same window, for a device whose bytes are the device itself: a register
 * of a controller. The page is uncached in both directions (PCD and PWT),
 * because a read of a register may be the read of the state of the device
 * rather than of the memory. A framebuffer is not mapped here: its stores may
 * be combined, and port_x86_64_paging_map_framebuffer() gives it WC through
 * PAT without changing the policy of any other window.
 */
k_u32_t port_x86_64_paging_map_mmio(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    const k_u64_t high_end = PORT_X86_64_KERNEL_BASE + 0x40000000ull;
    k_u64_t first = paddr & ~(k_u64_t)(LARGE_PAGE_SIZE - 1u);
    k_u64_t last = paddr + (k_u64_t)len;
    k_u64_t chunk;
    k_u32_t index = (k_u32_t)((vaddr >> 21) & (TABLE_ENTRIES - 1u));
    k_u32_t pages = 0;

    if (len == 0 || (vaddr & (k_u64_t)(LARGE_PAGE_SIZE - 1u)) != 0 ||
        vaddr < PORT_X86_64_KERNEL_BASE) {
        return 0;
    }

    if (vaddr + (last - first) > high_end) {
        return 0;
    }

    for (chunk = first; chunk < last; chunk += LARGE_PAGE_SIZE) {
        if (index >= TABLE_ENTRIES) {
            return 0;
        }

        g_kernel_pd_high[index] = (chunk & PTE_ADDRESS) | PTE_PRESENT | PTE_WRITABLE |
                                  PTE_PAGE_SIZE | PTE_NX | PTE_PCD | PTE_PWT;
        invlpg(PORT_X86_64_KERNEL_BASE + ((k_u64_t)index << 21));
        index++;
        pages++;
    }

    return pages;
}

/* Drop only the large-page entries of a named device window. No RAM frame is
 * released: the physical pages are BAR/aperture memory owned by the device. */
void port_x86_64_paging_unmap_device(k_u64_t vaddr, k_size_t len)
{
    const k_u64_t high_end = PORT_X86_64_KERNEL_BASE + 0x40000000ull;
    k_u64_t at;
    k_u64_t end;

    if (len == 0u || (vaddr & (k_u64_t)(LARGE_PAGE_SIZE - 1u)) != 0u ||
        vaddr < PORT_X86_64_KERNEL_BASE || vaddr >= high_end || len > high_end - vaddr) {
        return;
    }
    end = vaddr + (k_u64_t)len;
    for (at = vaddr; at < end; at += LARGE_PAGE_SIZE) {
        k_u32_t index = (k_u32_t)((at >> 21) & (TABLE_ENTRIES - 1u));

        g_kernel_pd_high[index] = 0u;
        invlpg(at);
    }
}

/*
 * The linear framebuffer, with a memory type of its own. PAT entry 5 is WC;
 * the large-page PTE selects it with PAT=1, PCD=0 and PWT=1. Only the named
 * entries of the shared high page directory change, so the identity map and
 * the uncached register windows keep the flags they already had. No variable
 * MTRR is consumed or reprogrammed.
 *
 * A processor without PAT, or one whose PAT read-back does not match, takes
 * the same PTE without bit 12: PCD=0 and PWT=1 is write-through memory there.
 * It is slower than WC but correct, and a missing feature is not a reason to
 * stop the machine. The PTE and the PAT/MTRR registers are read back and named
 * on COM1 so the smoke can check the policy it actually booted with.
 */
k_u32_t port_x86_64_paging_map_framebuffer(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    const k_u64_t high_end = PORT_X86_64_KERNEL_BASE + 0x40000000ull;
    const k_u64_t flag_mask = PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE | PTE_NX | PTE_CACHE_MASK;
    k_u64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE | PTE_NX | PTE_PWT;
    k_u64_t first = paddr & ~(k_u64_t)(LARGE_PAGE_SIZE - 1u);
    k_u64_t last = paddr + (k_u64_t)len;
    k_u64_t chunk;
    k_u32_t index = (k_u32_t)((vaddr >> 21) & (TABLE_ENTRIES - 1u));
    k_u32_t pages = 0;

    if (len == 0 || (vaddr & (k_u64_t)(LARGE_PAGE_SIZE - 1u)) != 0 ||
        vaddr < PORT_X86_64_KERNEL_BASE) {
        return 0;
    }

    if (vaddr + (last - first) > high_end) {
        return 0;
    }

    if (g_pat_wc_ready) {
        flags |= PTE_PAT_LARGE;
    }

    for (chunk = first; chunk < last; chunk += LARGE_PAGE_SIZE) {
        if (index >= TABLE_ENTRIES) {
            return 0;
        }

        g_kernel_pd_high[index] = (chunk & PTE_ADDRESS) | flags;

        if ((g_kernel_pd_high[index] & flag_mask) != (flags & flag_mask)) {
            kerror("screen: the framebuffer PTE did not read back as 0x%08x%08x",
                   (unsigned)((flags & flag_mask) >> 32), (unsigned)(flags & flag_mask));
            return 0;
        }

        invlpg(PORT_X86_64_KERNEL_BASE + ((k_u64_t)index << 21));
        index++;
        pages++;
    }

    if (g_pat_wc_ready) {
        kinfo("screen: framebuffer cache is write-combining, "
              "PAT=0x%08x%08x (entry %u=0x%02x), PTE flags=0x%08x%08x",
              (unsigned)(g_pat_value >> 32), (unsigned)g_pat_value, (unsigned)PAT_WC_ENTRY,
              (unsigned)pat_entry(g_pat_value, PAT_WC_ENTRY), (unsigned)((flags & flag_mask) >> 32),
              (unsigned)(flags & flag_mask));
    } else {
        kinfo("screen: framebuffer cache is write-through (no verified PAT), "
              "PTE flags=0x%08x%08x",
              (unsigned)((flags & flag_mask) >> 32), (unsigned)(flags & flag_mask));
    }

    if (g_mtrr_on) {
        k_u64_t mtrr = port_rdmsr(IA32_MTRR_DEF_TYPE_MSR);

        kinfo("screen: MTRR_DEF_TYPE=0x%08x%08x (unchanged)", (unsigned)(mtrr >> 32),
              (unsigned)mtrr);
    } else {
        kinfo("screen: no MTRR, the framebuffer type comes from PAT and the PTE");
    }

    return pages;
}

/*
 * The protection of one 4 KiB page of the kernel image, by the section it is
 * in. The text is readable and executable and never writable; the rodata is
 * readable, not writable and not executable; everything else - data, bss, the
 * stacks, the page tables and the heap - is writable and NX. A page that
 * holds the tail of a section (and the padding after it) takes the protection
 * of that section, which is why the linker separates the sections by whole
 * pages.
 */
static k_u64_t kernel_page_flags(k_u64_t address)
{
    k_u64_t page_end = address + PAGE_SIZE;
    k_u64_t text_start = (k_u64_t)(uintptr_t)__text_start;
    k_u64_t text_end = (k_u64_t)(uintptr_t)__text_end;
    k_u64_t rodata_start = (k_u64_t)(uintptr_t)__rodata_start;
    k_u64_t rodata_end = (k_u64_t)(uintptr_t)__rodata_end;

    if (address < text_end && page_end > text_start) {
        return PTE_PRESENT;
    }

    if (address < rodata_end && page_end > rodata_start) {
        return PTE_PRESENT | PTE_NX;
    }

    return PTE_PRESENT | PTE_WRITABLE | PTE_NX;
}

/*
 * Give the chunks of the map that carry the kernel image a table of 4 KiB
 * pages each, so that the text and the data of the same 2 MiB can have
 * different protections. The rest of the first gigabyte keeps its 2 MiB
 * pages, writable and NX: the VGA, the boot information, the heap and the
 * arena of the programs are data and nothing jumps into them.
 *
 * The image has two names: the physical address the loader copied the bytes
 * to, in the low identity map, and the link address at the top, where the
 * code runs. Both PD entries name the same table, so the protection of a
 * page is the same whichever one the walk came through.
 */
static void kernel_image_map(void)
{
    k_u64_t image_phys = page_phys(__text_start);
    k_u64_t image_virt = (k_u64_t)(uintptr_t)__text_start;
    k_u64_t image_start = image_phys & ~(k_u64_t)(LARGE_PAGE_SIZE - 1u);
    k_u64_t image_end =
        (page_phys(__kernel_end) + LARGE_PAGE_SIZE - 1u) & ~(k_u64_t)(LARGE_PAGE_SIZE - 1u);
    k_u64_t chunk;
    k_u32_t table = 0;

    for (chunk = image_start; chunk < image_end && table < KERNEL_IMAGE_TABLES;
         chunk += LARGE_PAGE_SIZE) {
        k_u64_t virtual_chunk = chunk - image_phys + image_virt;
        k_u32_t low_index = (k_u32_t)((chunk >> 21) & (TABLE_ENTRIES - 1u));
        k_u32_t high_index = (k_u32_t)((virtual_chunk >> 21) & (TABLE_ENTRIES - 1u));
        k_u32_t index;

        for (index = 0; index < TABLE_ENTRIES; index++) {
            k_u64_t physical = chunk + ((k_u64_t)index << PAGE_SHIFT);

            g_kernel_image_pt[table][index] =
                (physical & PTE_ADDRESS) |
                kernel_page_flags(virtual_chunk + ((k_u64_t)index) * PAGE_SIZE);
        }

        g_kernel_pd_low[low_index] =
            page_phys(g_kernel_image_pt[table]) | PTE_PRESENT | PTE_WRITABLE;
        g_kernel_pd_high[high_index] =
            page_phys(g_kernel_image_pt[table]) | PTE_PRESENT | PTE_WRITABLE;
        table++;
    }

    if (chunk < image_end) {
        /* The pool is the bound of the map, and an image that has outgrown it
         * would run with its text NX and stop at the first instruction. It is
         * named on COM1 instead, before the console of the kernel exists. */
        port_x86_64_panic("x86-64: the kernel image is too large for its page map\n");

        for (;;) {
            __asm__ volatile("hlt");
        }
    }
}

void port_x86_64_paging_init(void)
{
    k_u32_t index;

    /*
     * The identity map of the first gigabyte. The heap and the physical arena
     * of the programs are inside it, and the image of the kernel is reached
     * there as well until kernel_image_map() gives its chunks a table; a
     * 2 MiB page has no user bit, so ring 3 sees none of them even while its
     * own CR3 is the one in the register. Every one is NX until the text gets
     * its own table: data is never code on this machine.
     *
     * The map at the top starts empty and kernel_image_map() fills the chunks
     * of the image: the rest of the top 2 GiB is not memory of the kernel.
     */
    for (index = 0; index < TABLE_ENTRIES; index++) {
        g_kernel_pd_low[index] =
            ((k_u64_t)index << 21) | PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE | PTE_NX;
        g_kernel_pd_high[index] = 0;
    }

    /*
     * The first 2 MiB are split into 4 KiB pages so that the trampoline of
     * the application processors is executable and the page the BSP writes the
     * parameters to is not. Every other page of the chunk keeps the protection
     * of the large page it replaced: writable, supervisor and NX.
     */
    for (index = 0; index < TABLE_ENTRIES; index++) {
        k_u64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_NX;

        if (index == AP_TRAMPOLINE_PAGE) {
            /* Code the AP runs after paging is on: readable, executable and
             * never writable, so W^X holds for the kernel's own map too. */
            flags = PTE_PRESENT;
        }

        g_kernel_low_pt[index] = ((k_u64_t)index << PAGE_SHIFT) | flags;
    }

    g_kernel_pd_low[0] = page_phys(g_kernel_low_pt) | PTE_PRESENT | PTE_WRITABLE;

    kernel_image_map();

    g_kernel_pdpt_low[0] = page_phys(g_kernel_pd_low) | PTE_PRESENT | PTE_WRITABLE;
    g_kernel_pdpt_high[PDPT_HIGH_ENTRY] = page_phys(g_kernel_pd_high) | PTE_PRESENT | PTE_WRITABLE;

    /*
     * Two PML4 entries, two PDPTs: the low half is a map the port makes for
     * this address space (a program gets a copy of the PD), and the high half
     * is the image, shared by every space and supervisor-only.
     */
    g_kernel_pml4[0] = page_phys(g_kernel_pdpt_low) | PTE_PRESENT | PTE_WRITABLE;
    g_kernel_pml4[PML4_HIGH_ENTRY] = page_phys(g_kernel_pdpt_high) | PTE_PRESENT | PTE_WRITABLE;

    write_cr3(page_phys(g_kernel_pml4));
    g_paging_ready = true;

    /*
     * The kernel has to honour the write bit of its own tables; CR0 is a
     * register of the core, so every core makes that promise for itself
     * (port_x86_64_paging_init_cpu(), below).
     */
    port_x86_64_paging_init_cpu();
}

/*
 * The part of the map that is a register of one core: CR0.WP. A system call
 * writes the memory of the program (the status of waitpid, the buffer of a
 * read), and a page that a fork made read-only has to fault in ring 0 as
 * well, or the kernel would write into the frame the child is still reading.
 * With WP set the page fault handler sees the write and copies the page,
 * exactly as it does for a write of the program.
 *
 * The boot calls this for itself through port_x86_64_paging_init(); an
 * application processor calls it on its way up. A core that skipped it would
 * run every kernel copy-to-user without the check, and a program's text page
 * would take the write of a read() instead of going down with the fault that
 * follows it (the third fault of programs/nx.c, measured as an answer that
 * depended on the core).
 */
void port_x86_64_paging_init_cpu(void)
{
    k_u64_t cr0;
    k_u64_t cr4;
    bool smep;
    bool smap;
    bool pat;
    bool mtrr;

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 |= (k_u64_t)1u << 16; /* CR0.WP */
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

    /*
     * SMEP and SMAP (ADR 0052) are cleared here, not enabled. CPUID leaf 7
     * is only there when the highest basic leaf is at least 7; the bit 7 of
     * EBX is SMEP and the bit 20 is SMAP. A CR4 bit a processor does not
     * know is a general protection fault, so clearing the two is safe on
     * every machine, while a firmware that left them on is the case that
     * stopped the first program on a Ryzen desktop.
     */
    smep = false;
    smap = false;
    pat = false;
    mtrr = false;

    {
        k_u32_t eax;
        k_u32_t ebx;
        k_u32_t ecx;
        k_u32_t edx;
        k_u32_t max_leaf;

        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0u), "c"(0u));
        max_leaf = eax;

        if (max_leaf >= 1u) {
            __asm__ volatile("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(1u), "c"(0u));

            mtrr = ((edx >> 12) & 1u) != 0u;
            pat = ((edx >> 16) & 1u) != 0u;
        }

        if (max_leaf >= 7u) {
            __asm__ volatile("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(7u), "c"(0u));

            smep = ((ebx >> 7) & 1u) != 0u;
            smap = ((ebx >> 20) & 1u) != 0u;
        }
    }

    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));

    {
        k_u64_t before = cr4;

        cr4 &= ~(CR4_SMEP | CR4_SMAP);
        __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

        /*
         * The line is on the screen and COM1 before the first program runs,
         * so a machine whose firmware set the bits says what the port did
         * about it. Only the boot prints: an application processor makes the
         * same change to its own register in silence.
         */
        if (k_port_cpu_id() == 0u) {
            kinfo("x86-64: cr4 0x%08x%08x -> 0x%08x%08x (smep/smap off, advertised %d/%d)",
                  (unsigned)(before >> 32), (unsigned)before, (unsigned)(cr4 >> 32), (unsigned)cr4,
                  smep ? 1 : 0, smap ? 1 : 0);
        }
    }

    /* The CPUID answers are kept for the log; the bits are off by policy. */
    g_smep_on = false;
    g_smap_on = false;

    /*
     * PAT is per core too. Entry 5 is reserved for the framebuffer and the
     * other seven entries are preserved byte for byte. Writing the value even
     * when it already names WC makes the read-back a check of the register the
     * port relies on, not an assumption about the firmware's default.
     */
    g_mtrr_on = mtrr;
    g_pat_value = 0u;
    g_pat_wc_ready = false;

    if (pat) {
        k_u64_t next = pat_set_entry(port_rdmsr(IA32_PAT_MSR), PAT_WC_ENTRY, PAT_TYPE_WC);

        port_wrmsr(IA32_PAT_MSR, next);
        g_pat_value = port_rdmsr(IA32_PAT_MSR);
        g_pat_wc_ready = g_pat_value == next && pat_entry(g_pat_value, PAT_WC_ENTRY) == PAT_TYPE_WC;
    }
}

bool port_x86_64_smep_on(void)
{
    return g_smep_on;
}

bool port_x86_64_smap_on(void)
{
    return g_smap_on;
}

void port_x86_64_user_access_begin(void)
{
    if (g_smap_on) {
        __asm__ volatile("stac" : : : "memory");
    }
}

void port_x86_64_user_access_end(void)
{
    if (g_smap_on) {
        __asm__ volatile("clac" : : : "memory");
    }
}

k_u64_t port_x86_64_kernel_cr3(void)
{
    return page_phys(g_kernel_pml4);
}

k_u64_t port_x86_64_current_cr3(void)
{
    k_u64_t cr3;

    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return cr3;
}

k_u64_t port_x86_64_paging_space_create(void)
{
    k_u64_t *pml4;
    k_u64_t *pdpt;
    k_u64_t *pd;
    k_u64_t *low_pt;
    k_u32_t index;

    pml4 = page_pool_alloc();
    pdpt = page_pool_alloc();
    pd = page_pool_alloc();
    low_pt = page_pool_alloc();

    if (pml4 == NULL || pdpt == NULL || pd == NULL || low_pt == NULL) {
        goto fail;
    }

    /*
     * The low half of the kernel map is copied, not shared: the program's PD
     * replaces the entries of the window and the kernel's table must keep the
     * 2 MiB pages it was built with.
     */
    memcpy(pd, g_kernel_pd_low, sizeof(g_kernel_pd_low));

    /*
     * The first 2 MiB stay reachable for the kernel - the console writes the
     * text memory at 0xB8000 during a system call of the program - but the
     * zero page itself is not a page: a null pointer of a program faults, and
     * the rest of the low memory is supervisor-only, so ring 3 cannot read it
     * even though it is mapped.
     */
    for (index = 0; index < TABLE_ENTRIES; index++) {
        if (index == 0) {
            low_pt[index] = 0;
        } else {
            low_pt[index] = ((k_u64_t)index << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITABLE | PTE_NX;
        }
    }

    pd[0] = page_phys(low_pt) | PTE_PRESENT | PTE_WRITABLE;

    /*
     * The window starts with no table at all: a chunk of 2 MiB gets its table
     * of 4 KiB pages on the first page of that chunk that is mapped
     * (port_x86_64_paging_map_user). A program that uses one megabyte of
     * window costs three tables and not the forty-eight of 96 MiB, and a
     * program that uses all of it costs them all - the same number it would
     * have paid eagerly. The copy of the kernel's low map above has the
     * identity entries of the window, and they are cleared here: a program
     * may not name the physical memory behind its own window.
     */
    for (index = 0; index < USER_PD_COUNT; index++) {
        pd[USER_PD_FIRST + index] = 0;
    }

    /*
     * The user bit is checked at every level of the walk, not only at the
     * page: the two upper entries of the program's map open the door for the
     * window, and the kernel entries of the PD keep it shut for everything
     * else. A copy of the kernel's own upper entries would refuse ring 3
     * before it ever looked at the leaf.
     */
    pdpt[0] = page_phys(pd) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    pml4[0] = page_phys(pdpt) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /*
     * The high half of the map is the kernel's own, shared and not copied: a
     * program runs on this CR3 only while it is inside a call, and the code
     * it returns to, the GDT, the TSS, the stacks and the tables of the port
     * are all up there. Without the shared entry the first call of a program
     * would jump to an address that its space does not map. It carries no
     * user bit at any level, so ring 3 still cannot read a byte of it.
     */
    pml4[PML4_HIGH_ENTRY] = page_phys(g_kernel_pdpt_high) | PTE_PRESENT | PTE_WRITABLE;

    return page_phys(pml4);

fail:
    /* Every table that was handed out above is this function's to give
     * back; the ones that were never asked for are NULL. */
    if (pml4 != NULL) {
        page_pool_free(pml4);
    }
    if (pdpt != NULL) {
        page_pool_free(pdpt);
    }
    if (pd != NULL) {
        page_pool_free(pd);
    }
    if (low_pt != NULL) {
        page_pool_free(low_pt);
    }

    return 0;
}

/*
 * The address space of the child of a fork. The tables are a copy; the pages
 * of the window are the same frames, made read-only in both spaces and
 * marked as copy-on-write. The caller is the parent, so the parent's CR3 is
 * the one in the register and its TLB entries can be dropped here.
 */
k_u64_t port_x86_64_paging_space_clone(k_u64_t parent_cr3, k_u32_t *out_shared)
{
    k_u64_t child_cr3;
    k_u64_t *parent_pd;
    k_u64_t *child_pd;
    k_u32_t pd_index;
    k_u32_t shared = 0;

    if (parent_cr3 == 0) {
        return 0;
    }

    child_cr3 = port_x86_64_paging_space_create();
    if (child_cr3 == 0) {
        return 0;
    }

    parent_pd = space_pd(parent_cr3);
    child_pd = space_pd(child_cr3);

    if (parent_pd == NULL || child_pd == NULL) {
        port_x86_64_paging_space_destroy(child_cr3);
        return 0;
    }

    /*
     * The walk is one critical section: the frames are a state of the machine
     * and a fork of a program of 64 MiB names sixteen thousand of them. The
     * tables are copied whole - a chunk of 2 MiB is 512 entries and one
     * memcpy - and the pages are not touched at all: the child gets the same
     * frames, and the first write of either side copies the one page it
     * wrote (port_x86_64_paging_handle_cow). The cost of the call is the
     * tables of the parent and the count of its frames, and not their
     * contents.
     */
    k_port_enter_critical();

    for (pd_index = 0; pd_index < USER_PD_COUNT; pd_index++) {
        k_u64_t *parent_pt = space_pt(parent_cr3, USER_PD_FIRST + pd_index);
        k_u64_t *child_pt;
        k_u32_t pt_index;

        if (parent_pt == NULL) {
            continue;
        }

        child_pt = page_pool_take(false);
        if (child_pt == NULL) {
            k_port_exit_critical();
            port_x86_64_paging_space_destroy(child_cr3);
            return 0;
        }

        /*
         * The table of the parent is made the shared table first - the count
         * of every frame and the copy-on-write mark - and the child gets it
         * by the memcpy: one store per page instead of two, and no zeroing of
         * a page that the copy fills.
         */
        for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
            k_u64_t entry = parent_pt[pt_index];

            if ((entry & PTE_PRESENT) == 0) {
                continue;
            }

            /* The frame is shared by the two spaces, even when it is the text:
             * the last space to leave it is the one that releases it. */
            frame_ref_locked(entry & PTE_ADDRESS);
            shared++;

            /*
             * Only a page that was writable becomes copy-on-write. The text
             * of a program is read-only before the fork, and a fork does not
             * change that: clearing a bit it did not have and marking the
             * page COW would turn the first write into a private writable
             * copy, and a program could then rewrite its own code (the W^X
             * rule of ADR 0007).
             */
            if ((entry & PTE_WRITABLE) != 0) {
                /* The complement has to be 64 bits wide: a `~PTE_WRITABLE` of
                 * the 32-bit constant is zero-extended and takes the NX bit
                 * of the entry with it. */
                parent_pt[pt_index] = (entry & ~(k_u64_t)PTE_WRITABLE) | PTE_COW;
            }
        }

        memcpy(child_pt, parent_pt, PAGE_SIZE);
        child_pd[USER_PD_FIRST + pd_index] =
            page_phys(child_pt) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    k_port_exit_critical();

    /*
     * The parent's own entries changed under it. A flush of the whole TLB (a
     * write of CR3, which is the parent's) costs one entry of the pipeline
     * and not one `invlpg` per page: sixteen thousand of those were the
     * larger half of the call. The port writes CR3 at every context switch
     * anyway, so no core holds a stale entry of this space for longer than
     * that (the K8 debt of the TLB, docs/debts.md).
     */
    write_cr3(parent_cr3);

    if (out_shared != NULL) {
        *out_shared = shared;
    }

    /*
     * The memory of the child is the memory of the parent: the records of the
     * mappings go with the new tables, and the pages the child has not
     * touched are still pages of the files behind them. The call opens files
     * and takes the lock of the kernel heap, so it is made outside the
     * critical section of the tables above.
     */
    (void)k_mm_fork(parent_cr3, child_cr3);

    return child_cr3;
}

void port_x86_64_paging_space_destroy(k_u64_t cr3)
{
    k_u64_t *pml4;

    /*
     * The mappings of the space are records of the kernel (kernel/mm.h) and
     * they go with the tables: the files of a mapping are closed here, before
     * the frames the records name are given back.
     */
    k_mm_release(cr3);

    k_u64_t *pdpt;
    k_u64_t *pd;
    k_u64_t *low_pt;
    k_u32_t pd_index;

    if (cr3 == 0 || cr3 == page_phys(g_kernel_pml4)) {
        return;
    }

    pml4 = (k_u64_t *)(uintptr_t)cr3;
    if ((pml4[0] & PTE_PRESENT) == 0) {
        return;
    }

    pdpt = table_entry(pml4[0]);
    if ((pdpt[0] & PTE_PRESENT) == 0) {
        return;
    }

    pd = table_entry(pdpt[0]);
    low_pt = (pd[0] & PTE_PRESENT) && (pd[0] & PTE_PAGE_SIZE) == 0 ? table_entry(pd[0]) : NULL;

    /*
     * The frames of the window belong to the spaces, not to the kernel: the
     * last one to leave releases the page. The walk is one critical section,
     * for the reason the clone is: a program of 64 MiB leaves sixteen
     * thousand frames, and the lock of each of them was the price of leaving.
     */
    k_port_enter_critical();

    for (pd_index = 0; pd_index < USER_PD_COUNT; pd_index++) {
        if ((pd[USER_PD_FIRST + pd_index] & PTE_PRESENT) != 0 &&
            (pd[USER_PD_FIRST + pd_index] & PTE_PAGE_SIZE) == 0) {
            k_u64_t *pt = table_entry(pd[USER_PD_FIRST + pd_index]);
            k_u32_t pt_index;

            for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
                if ((pt[pt_index] & PTE_PRESENT) != 0) {
                    frame_release_locked(pt[pt_index] & PTE_ADDRESS);
                }
            }

            page_pool_free(pt);
        }
    }

    k_port_exit_critical();

    if (low_pt != NULL) {
        page_pool_free(low_pt);
    }

    page_pool_free(pd);
    page_pool_free(pdpt);
    page_pool_free(pml4);
}

int port_x86_64_paging_map_user(k_u64_t cr3, k_u64_t vaddr, k_u64_t paddr, k_size_t len,
                                k_u32_t flags)
{
    k_u64_t address;
    k_u64_t entry_flags = PTE_PRESENT | PTE_USER;
    int rc = 0;

    if ((flags & PORT_X86_64_PAGE_WRITE) != 0) {
        entry_flags |= PTE_WRITABLE;
    }

    if ((flags & PORT_X86_64_PAGE_EXEC) == 0) {
        entry_flags |= PTE_NX;
    }

    if (cr3 == 0 || (vaddr & (PAGE_SIZE - 1u)) != 0 || (paddr & (PAGE_SIZE - 1u)) != 0 ||
        (len & (PAGE_SIZE - 1u)) != 0) {
        return -1;
    }

    k_port_enter_critical();

    for (address = 0; address < len; address += PAGE_SIZE) {
        k_u64_t virtual_address = vaddr + address;
        k_u64_t physical_address = paddr + address;
        k_u64_t pd_index;
        k_u64_t pt_index;
        k_u64_t *pt;

        if (virtual_address < PORT_X86_64_USER_BASE || virtual_address >= PORT_X86_64_USER_LIMIT) {
            rc = -1;
            break;
        }

        pd_index = (virtual_address >> 21) & (TABLE_ENTRIES - 1u);
        pt_index = (virtual_address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
        pt = space_pt(cr3, pd_index);

        if (pt == NULL) {
            /*
             * The first page of a chunk of 2 MiB: the table comes now, from
             * the pool, and the entry of the program's PD opens the chunk to
             * ring 3 (the user bit is checked at every level). A chunk that
             * holds a 2 MiB page is not a chunk of this window - the port
             * maps it in 4 KiB pages only - and is refused rather than
             * overwritten.
             */
            k_u64_t *pd = space_pd(cr3);

            if (pd == NULL || (pd[pd_index] & PTE_PRESENT) != 0) {
                rc = -1;
                break;
            }

            pt = page_pool_alloc();
            if (pt == NULL) {
                rc = -1;
                break;
            }

            pd[pd_index] = page_phys(pt) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }

        pt[pt_index] = (physical_address & ~(k_u64_t)(PAGE_SIZE - 1u)) | entry_flags;
    }

    k_port_exit_critical();

    return rc;
}

/*
 * A store of the kernel into a read-only page of a program. The kernel copies
 * to the buffer of a call under the page tables of the caller, and a program
 * that hands its own text as a buffer asks for a page that is code to take a
 * write. The page cannot stay code and take the store at once: this space
 * gets a private copy of it, writable and NX, which is the copy-on-write rule
 * of a fork used for the other direction. The frame is copied and not
 * changed in place, because a fork shares the code with the child and a write
 * of one space must not rewrite the instructions of the other. The program
 * meets the missing code on its way back to ring 3, where a fetch from it is
 * a fault of the program and not of the machine. Returns 1 when the
 * instruction that faulted may be retried.
 */
int port_x86_64_paging_allow_kernel_write(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pt;
    k_u64_t entry;
    k_u64_t old_frame;
    k_u64_t new_frame;
    int rc = 0;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return 0;
    }

    k_port_enter_critical();

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);

    if (pt == NULL) {
        goto out;
    }

    entry = pt[pt_index];

    /* Only a page of the program that is there, open to ring 3 and not
     * writable is this case; a kernel page of rodata is a bug of the kernel
     * and the machine stops there, as it always did. */
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_WRITABLE)) != (PTE_PRESENT | PTE_USER)) {
        goto out;
    }

    old_frame = entry & PTE_ADDRESS;
    new_frame = port_x86_64_frame_alloc();
    if (new_frame == 0) {
        goto out;
    }

    memcpy((void *)(uintptr_t)new_frame, (const void *)(uintptr_t)old_frame, PAGE_SIZE);

    pt[pt_index] = new_frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    frame_release_locked(old_frame);
    rc = 1;

out:
    k_port_exit_critical();
    return rc;
}

/*
 * A page of a program that is not there yet: a frame of the arena, mapped at
 * the address with the protection of the kernel, and the address of the
 * kernel that holds the bytes - the identity map names every frame of the
 * arena, so the kernel writes the page it just made for the program.
 */
void *port_x86_64_paging_map_page(k_u64_t cr3, k_u64_t vaddr, k_u32_t flags)
{
    k_u64_t frame = port_x86_64_frame_alloc();
    void *page;

    if (frame == 0) {
        return NULL;
    }

    if (port_x86_64_paging_map_user(cr3, vaddr & ~(k_u64_t)(PAGE_SIZE - 1u), frame, PAGE_SIZE,
                                    flags) != 0) {
        port_x86_64_frame_release(frame);
        return NULL;
    }

    page = port_x86_64_paging_phys(cr3, vaddr);

    if (page == NULL) {
        port_x86_64_paging_unmap_page(cr3, vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));
        return NULL;
    }

    return page;
}

/* The page is there and the program wrote it: the write bit of the entry
 * comes back and the entry of the TLB of this core goes. */
bool port_x86_64_paging_page_writable(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    k_u64_t pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    k_u64_t *pt;
    k_u64_t entry;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return false;
    }

    pt = space_pt(cr3, pd_index);
    if (pt == NULL) {
        return false;
    }

    entry = pt[pt_index];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
        return false;
    }

    if ((entry & PTE_WRITABLE) == 0) {
        pt[pt_index] = entry | PTE_WRITABLE;
        invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));
    }

    return true;
}

/* A private copy of a page that is there: a frame of this space with the
 * bytes of the old one, writable, and the old frame released. A private
 * mapping of a file never needs it (the frame of a fault is already private),
 * but the copy of a page a fork shared does, and so does a page of a program
 * that the kernel is about to write. */
bool port_x86_64_paging_page_private(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pt;
    k_u64_t entry;
    k_u64_t old_frame;
    k_u64_t new_frame;
    bool rc = false;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return false;
    }

    k_port_enter_critical();

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);
    if (pt == NULL) {
        goto out;
    }

    entry = pt[pt_index];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
        goto out;
    }

    old_frame = entry & PTE_ADDRESS;
    new_frame = port_x86_64_frame_alloc();
    if (new_frame == 0) {
        goto out;
    }

    memcpy((void *)(uintptr_t)new_frame, (const void *)(uintptr_t)old_frame, PAGE_SIZE);

    pt[pt_index] = new_frame | (entry & PTE_NX) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    frame_release_locked(old_frame);
    rc = true;

out:
    k_port_exit_critical();
    return rc;
}

/* Unmap one page and give its frame back. */
void port_x86_64_paging_unmap_page(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pt;
    k_u64_t entry;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return;
    }

    k_port_enter_critical();

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);
    if (pt == NULL) {
        k_port_exit_critical();
        return;
    }

    entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        k_port_exit_critical();
        return;
    }

    pt[pt_index] = 0;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    frame_release_locked(entry & PTE_ADDRESS);
    k_port_exit_critical();
}

/*
 * True when an address of the kernel's half - the image, the heap window of
 * the port, the window of a device - is in the map of every space: the top
 * 2 GiB are one table shared by all of them (the comment above
 * port_x86_64_paging_space_create), so the answer is the entry of the kernel's
 * own table and not the walk of a space. A driver that polls a card asks this
 * before it reads a register: a window that a space lost is a page fault of
 * the kernel and not a frame lost.
 */
bool port_x86_64_paging_ready(void)
{
    return g_paging_ready;
}

bool port_x86_64_paging_kernel_present(k_u64_t vaddr)
{
    k_u64_t index;

    if (vaddr < PORT_X86_64_KERNEL_BASE) {
        return false;
    }

    index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);

    return (g_kernel_pd_high[index] & PTE_PRESENT) != 0u;
}

void *port_x86_64_paging_phys(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pt;
    k_u64_t entry;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return NULL;
    }

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);

    if (pt == NULL) {
        return NULL;
    }

    entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return NULL;
    }

    return (void *)(uintptr_t)((entry & PTE_ADDRESS) | (vaddr & (PAGE_SIZE - 1u)));
}

/*
 * The address a device sees for a buffer of the kernel or of a program
 * (ADR 0032 §1): the translation of the one window whose virtual address is
 * not its physical one, and the arithmetic of the two halves of the map for
 * everything else. Zero is a page that is not there, which no driver may give
 * to a device.
 */
k_u64_t port_x86_64_dma_phys(const void *buffer)
{
    k_u64_t address = (k_u64_t)(uintptr_t)buffer;
    void *frame;

    if (address >= PORT_X86_64_USER_BASE && address < PORT_X86_64_USER_LIMIT) {
        frame = port_x86_64_paging_phys(port_x86_64_current_cr3(),
                                        address & ~(k_u64_t)(PAGE_SIZE - 1u));

        if (frame == NULL) {
            return 0;
        }

        return (k_u64_t)(uintptr_t)frame | (address & (PAGE_SIZE - 1u));
    }

    return port_x86_64_bus_phys(buffer);
}

/*
 * The first write into a page a fork made read-only. The frame is copied and
 * the entry of this space points at the copy with the write bit back; the
 * other space keeps the old frame, which is now private to it too. Returns 1
 * when the instruction that faulted may be run again, 0 when this space never
 * asked for a copy (a real fault of the program).
 */
int port_x86_64_paging_handle_cow(k_u64_t cr3, k_u64_t vaddr)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pt;
    k_u64_t entry;
    k_u64_t old_frame;
    k_u64_t new_frame;
    int rc = 0;

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return 0;
    }

    k_port_enter_critical();

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);

    if (pt == NULL) {
        goto out;
    }

    entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0 || (entry & PTE_COW) == 0) {
        goto out;
    }

    old_frame = entry & PTE_ADDRESS;
    new_frame = port_x86_64_frame_alloc();
    if (new_frame == 0) {
        goto out;
    }

    memcpy((void *)(uintptr_t)new_frame, (const void *)(uintptr_t)old_frame, PAGE_SIZE);

    /*
     * The copy takes the protection of the page it replaces: a stack or a
     * variable stays NX after the first write, and only a page that was
     * executable before the fork could carry no NX bit. The private copy is
     * writable now, which is the point of the fault.
     */
    pt[pt_index] = new_frame | (entry & PTE_NX) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    frame_release_locked(old_frame);
    port_x86_64_task_cow_copy();
    rc = 1;

out:
    k_port_exit_critical();
    if (rc != 0) {
        klog(K_LOG_LEVEL_DEBUG, "x86-64: copy on write of the page at 0x%08x%08x",
             (unsigned)(vaddr >> 32), (unsigned)(vaddr & 0xFFFFFFFFu));
    }
    return rc;
}

/*
 * The 4 KiB table of the low identity map that holds `vaddr`. The chunk is
 * split once: every page keeps the protection of the large page it replaced
 * (the identity map of the heap is writable, supervisor and NX), and the
 * kernel's PD names the new table. NULL when the address is not in the
 * identity map or the pool of the tables has no page.
 */
static k_u64_t *kernel_low_pt(k_u64_t vaddr)
{
    k_u32_t index = (k_u32_t)((vaddr >> 21) & (TABLE_ENTRIES - 1u));
    k_u64_t entry;
    k_u64_t *pt;
    k_u64_t base;
    k_u32_t page;

    if (vaddr >= PORT_X86_64_IDENTITY_LIMIT ||
        (index >= USER_PD_FIRST && index < USER_PD_FIRST + USER_PD_COUNT)) {
        return NULL;
    }

    entry = g_kernel_pd_low[index];
    if ((entry & PTE_PRESENT) == 0) {
        return NULL;
    }

    if ((entry & PTE_PAGE_SIZE) == 0) {
        return table_entry(entry);
    }

    pt = page_pool_alloc();
    if (pt == NULL) {
        return NULL;
    }

    base = entry & PTE_ADDRESS;

    for (page = 0; page < TABLE_ENTRIES; page++) {
        k_u64_t flags =
            entry & (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT | PTE_NX);

        pt[page] = (base + ((k_u64_t)page << PAGE_SHIFT)) | flags;
    }

    g_kernel_pd_low[index] =
        page_phys(pt) | (entry & (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT));

    return pt;
}

bool port_x86_64_paging_kernel_guard_install(k_u64_t address)
{
    k_u64_t page = address & ~(k_u64_t)(PAGE_SIZE - 1u);
    k_u64_t *pt;
    k_u64_t entry;
    k_u32_t index;

    if (page < PAGE_SIZE || page >= PORT_X86_64_IDENTITY_LIMIT) {
        return false;
    }

    pt = kernel_low_pt(page);
    if (pt == NULL) {
        return false;
    }

    index = (k_u32_t)((page >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u));
    entry = pt[index];

    /*
     * Only a page of data may become the guard: a page of text is read-only
     * or executable, and a page that is already not there belongs to another
     * guard. The test is the whole of the promise the identity map makes for
     * the heap.
     */
    if ((entry & (PTE_PRESENT | PTE_WRITABLE | PTE_NX)) != (PTE_PRESENT | PTE_WRITABLE | PTE_NX)) {
        return false;
    }

    pt[index] = 0;
    invlpg(page);

    return true;
}

void port_x86_64_paging_kernel_guard_remove(k_u64_t address)
{
    k_u64_t page = address & ~(k_u64_t)(PAGE_SIZE - 1u);
    k_u64_t *pt;
    k_u32_t index;

    if (page < PAGE_SIZE || page >= PORT_X86_64_IDENTITY_LIMIT) {
        return;
    }

    pt = kernel_low_pt(page);
    if (pt == NULL) {
        return;
    }

    index = (k_u32_t)((page >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u));
    pt[index] = page | PTE_PRESENT | PTE_WRITABLE | PTE_NX;
    invlpg(page);
}

void port_x86_64_paging_space_sync_kernel(k_u64_t cr3, k_u64_t vaddr)
{
    k_u32_t index = (k_u32_t)((vaddr >> 21) & (TABLE_ENTRIES - 1u));
    k_u64_t *pd;

    if (cr3 == 0 || vaddr >= PORT_X86_64_IDENTITY_LIMIT || index == 0u ||
        (index >= USER_PD_FIRST && index < USER_PD_FIRST + USER_PD_COUNT)) {
        return;
    }

    pd = space_pd(cr3);
    if (pd == NULL) {
        return;
    }

    pd[index] = g_kernel_pd_low[index];
}

/*
 * Count the pages of one space that are both writable and executable. The
 * machine's tables are built by this file, and the audit is the check that
 * the protection the port promises is the protection the tables carry: the
 * kernel calls it on its own map at the boot, and port_exec.c on the map of a
 * program before the first instruction. Zero is the answer of a W^X map.
 *
 * Every PML4 entry is walked, not only the low one: since the kernel moved to
 * the higher half a map has two halves, and the high half is where the text
 * of the kernel lives. The chunks of the image share one table between the
 * two halves by design, so a leaf may be counted twice; a count that is zero
 * either way is the property, and a non-zero one is a bug.
 */
k_u32_t port_x86_64_paging_audit(k_u64_t cr3)
{
    const k_u64_t *pml4 = (const k_u64_t *)(uintptr_t)cr3;
    k_u32_t count = 0;
    k_u32_t pm;
    k_u32_t pdpt_index;
    k_u32_t pd_index;
    k_u32_t pt_index;

    if (pml4 == NULL) {
        return 0;
    }

    for (pm = 0; pm < TABLE_ENTRIES; pm++) {
        const k_u64_t *pdpt;

        if ((pml4[pm] & PTE_PRESENT) == 0) {
            continue;
        }

        pdpt = table_entry(pml4[pm]);

        for (pdpt_index = 0; pdpt_index < TABLE_ENTRIES; pdpt_index++) {
            const k_u64_t *pd;

            if ((pdpt[pdpt_index] & PTE_PRESENT) == 0) {
                continue;
            }

            if ((pdpt[pdpt_index] & PTE_PAGE_SIZE) != 0) {
                /* A 1 GiB page of a PDPT: the port builds none, and one that
                 * is here is a table the audit has to judge anyway. */
                if ((pdpt[pdpt_index] & PTE_WRITABLE) != 0 && (pdpt[pdpt_index] & PTE_NX) == 0) {
                    count++;
                }

                continue;
            }

            pd = table_entry(pdpt[pdpt_index]);

            for (pd_index = 0; pd_index < TABLE_ENTRIES; pd_index++) {
                k_u64_t entry = pd[pd_index];

                if ((entry & PTE_PRESENT) == 0) {
                    continue;
                }

                if ((entry & PTE_PAGE_SIZE) != 0) {
                    if ((entry & PTE_WRITABLE) != 0 && (entry & PTE_NX) == 0) {
                        count++;
                    }

                    continue;
                }

                {
                    const k_u64_t *pt = table_entry(entry);

                    for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
                        k_u64_t leaf = pt[pt_index];

                        if ((leaf & PTE_PRESENT) != 0 && (leaf & PTE_WRITABLE) != 0 &&
                            (leaf & PTE_NX) == 0) {
                            count++;
                        }
                    }
                }
            }
        }
    }

    return count;
}

void port_x86_64_paging_switch(k_u64_t cr3)
{
    write_cr3(cr3);
}
void port_x86_64_paging_debug_fault(k_u64_t cr3, k_u64_t va, k_u64_t error, k_u64_t rip,
                                    k_u32_t vector, k_u32_t pid)
{
    k_u64_t pd_index;
    k_u64_t pt_index;
    k_u64_t *pd;
    k_u64_t *pt;
    k_u64_t pde = 0;
    k_u64_t pte = 0;
    k_u16_t ref = 0;
    k_u32_t cpu = k_port_cpu_id();
    k_u32_t ticks = k_sched_ticks();

    pd_index = (va >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (va >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);

    k_port_enter_critical();
    pd = space_pd(cr3);
    if (pd != NULL) {
        pde = pd[pd_index];
        if ((pde & PTE_PRESENT) != 0 && (pde & PTE_PAGE_SIZE) == 0) {
            pt = table_entry(pde);
            pte = pt[pt_index];
            if ((pte & PTE_PRESENT) != 0) {
                k_size_t index;
                if (frame_index(pte & PTE_ADDRESS, &index)) {
                    ref = g_frame_ref[index];
                }
            }
        }
    }
    k_port_exit_critical();

    klog(K_LOG_LEVEL_ERROR, "pgf1: cpu=%u pid=%u tk=%u cr3=%08x%08x va=%08x%08x err=%08x%08x",
         (unsigned)cpu, (unsigned)pid, (unsigned)ticks, (unsigned)(cr3 >> 32),
         (unsigned)(cr3 & 0xFFFFFFFFu), (unsigned)(va >> 32), (unsigned)(va & 0xFFFFFFFFu),
         (unsigned)(error >> 32), (unsigned)(error & 0xFFFFFFFFu));
    klog(K_LOG_LEVEL_ERROR,
         "pgf2: rip=%08x%08x vec=%u pde=%08x%08x pte=%08x%08x P%uU%uW%uC%uX%u ref=%u",
         (unsigned)(rip >> 32), (unsigned)(rip & 0xFFFFFFFFu), (unsigned)vector,
         (unsigned)(pde >> 32), (unsigned)(pde & 0xFFFFFFFFu), (unsigned)(pte >> 32),
         (unsigned)(pte & 0xFFFFFFFFu), (unsigned)((pte & PTE_PRESENT) != 0),
         (unsigned)((pte & PTE_USER) != 0), (unsigned)((pte & PTE_WRITABLE) != 0),
         (unsigned)((pte & PTE_COW) != 0), (unsigned)((pte & PTE_NX) != 0), (unsigned)ref);
}
