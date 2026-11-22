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
#define PTE_PAGE_SIZE 0x080u /* in a PD entry: a 2 MiB page */

/*
 * A page that both the parent and the child of a fork may still read and
 * neither may write: the entry of the PTE that says "copy it on the first
 * write". The bit is free for the operating system, and the processor
 * ignores it.
 */
#define PTE_COW 0x200u

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
#define PAGE_POOL_PAGES 256u
#define PAGE_POOL_WORDS (PAGE_POOL_PAGES / 64u)

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

static void *page_pool_alloc(void)
{
    k_u64_t flags = port_save_flags_and_cli();
    k_u32_t word;
    void *page = NULL;

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
                memset(found, 0, PAGE_SIZE);
                page = found;
                break;
            }
        }
    }

    port_restore_flags(flags);
    return page;
}

static void page_pool_free(void *page)
{
    k_u8_t *base = pool_page((k_u8_t *)page);
    k_size_t index;
    k_u32_t word;
    k_u32_t bit;
    k_u64_t flags;

    if (base < g_page_pool || base >= g_page_pool + sizeof(g_page_pool)) {
        return;
    }

    index = (k_size_t)(base - g_page_pool) / PAGE_SIZE;
    word = (k_u32_t)(index / 64u);
    bit = (k_u32_t)(index % 64u);

    flags = port_save_flags_and_cli();
    g_page_bitmap[word] &= ~((k_u64_t)1u << bit);
    port_restore_flags(flags);
}

/* --- the frames of the physical arena ------------------------------------- */

static void frame_ref(k_u64_t address)
{
    k_size_t index;

    if (address < PORT_X86_64_USER_PHYS ||
        address >= PORT_X86_64_USER_PHYS + USER_FRAME_COUNT * PAGE_SIZE) {
        return;
    }

    index = (k_size_t)(address - PORT_X86_64_USER_PHYS) / PAGE_SIZE;
    if (g_frame_ref[index] != 0xFFFFu) {
        g_frame_ref[index]++;
    }
}

k_u64_t port_x86_64_frame_alloc(void)
{
    k_u64_t flags = port_save_flags_and_cli();
    k_size_t index;
    k_u64_t address = 0;

    for (index = 0; index < USER_FRAME_COUNT; index++) {
        if (g_frame_ref[index] == 0) {
            g_frame_ref[index] = 1;
            address = PORT_X86_64_USER_PHYS + (k_u64_t)index * PAGE_SIZE;
            break;
        }
    }

    port_restore_flags(flags);

    if (address != 0) {
        /* A new page of a program reads as zeros, not as the stack of the
         * program that had the frame before it. */
        memset((void *)(uintptr_t)address, 0, PAGE_SIZE);
    }

    return address;
}

void port_x86_64_frame_release(k_u64_t address)
{
    k_size_t index;
    k_u64_t flags;

    if (address < PORT_X86_64_USER_PHYS ||
        address >= PORT_X86_64_USER_PHYS + USER_FRAME_COUNT * PAGE_SIZE) {
        return;
    }

    index = (k_size_t)(address - PORT_X86_64_USER_PHYS) / PAGE_SIZE;

    flags = port_save_flags_and_cli();
    if (g_frame_ref[index] != 0) {
        g_frame_ref[index]--;
    }
    port_restore_flags(flags);
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

/*
 * A window of a device in the map of the kernel: 2 MiB pages of the top 2 GiB
 * that name the physical range of the device, writable and NX. The screen of
 * ADR 0023 is the first user - the framebuffer of the VGA sits at 0xFD000000,
 * above the identity map of the first gigabyte - and the window is the
 * kernel's own: every address space shares the PDPT of the top 2 GiB, so a
 * store of a system call reaches the picture whichever space the processor is
 * on.
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

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 |= (k_u64_t)1u << 16; /* CR0.WP */
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
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
    k_u64_t *pt[USER_PD_COUNT] = {0};
    k_u32_t index;

    pml4 = page_pool_alloc();
    pdpt = page_pool_alloc();
    pd = page_pool_alloc();
    low_pt = page_pool_alloc();

    if (pml4 == NULL || pdpt == NULL || pd == NULL || low_pt == NULL) {
        goto fail;
    }

    for (index = 0; index < USER_PD_COUNT; index++) {
        pt[index] = page_pool_alloc();
        if (pt[index] == NULL) {
            goto fail;
        }
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

    for (index = 0; index < USER_PD_COUNT; index++) {
        pd[USER_PD_FIRST + index] = page_phys(pt[index]) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
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
    for (index = 0; index < USER_PD_COUNT; index++) {
        if (pt[index] != NULL) {
            page_pool_free(pt[index]);
        }
    }

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
k_u64_t port_x86_64_paging_space_clone(k_u64_t parent_cr3)
{
    k_u64_t child_cr3;
    k_u64_t *parent_pd;
    k_u64_t *child_pd;
    k_u32_t pd_index;

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

    for (pd_index = 0; pd_index < USER_PD_COUNT; pd_index++) {
        k_u64_t *parent_pt = space_pt(parent_cr3, USER_PD_FIRST + pd_index);
        k_u64_t *child_pt = space_pt(child_cr3, USER_PD_FIRST + pd_index);
        k_u32_t pt_index;

        if (parent_pt == NULL || child_pt == NULL) {
            port_x86_64_paging_space_destroy(child_cr3);
            return 0;
        }

        for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
            k_u64_t entry = parent_pt[pt_index];
            k_u64_t address;

            if ((entry & PTE_PRESENT) == 0) {
                continue;
            }

            address = (k_u64_t)PORT_X86_64_USER_BASE + ((k_u64_t)(USER_PD_FIRST + pd_index) << 21) +
                      ((k_u64_t)pt_index << PAGE_SHIFT);

            /* The frame is shared by the two spaces, even when it is the text:
             * the last space to leave it is the one that releases it. */
            frame_ref(entry & PTE_ADDRESS);

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
                k_u64_t shared = entry & ~(k_u64_t)PTE_WRITABLE;

                child_pt[pt_index] = shared | PTE_COW;
                parent_pt[pt_index] = shared | PTE_COW;

                /* The parent wrote through this entry before the fork. */
                invlpg(address);
            } else {
                child_pt[pt_index] = entry;
            }
        }
    }

    return child_cr3;
}

void port_x86_64_paging_space_destroy(k_u64_t cr3)
{
    k_u64_t *pml4;
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

    /* The frames of the window belong to the spaces, not to the kernel: the
     * last one to leave releases the page. */
    for (pd_index = 0; pd_index < USER_PD_COUNT; pd_index++) {
        if ((pd[USER_PD_FIRST + pd_index] & PTE_PRESENT) != 0 &&
            (pd[USER_PD_FIRST + pd_index] & PTE_PAGE_SIZE) == 0) {
            k_u64_t *pt = table_entry(pd[USER_PD_FIRST + pd_index]);
            k_u32_t pt_index;

            for (pt_index = 0; pt_index < TABLE_ENTRIES; pt_index++) {
                if ((pt[pt_index] & PTE_PRESENT) != 0) {
                    port_x86_64_frame_release(pt[pt_index] & PTE_ADDRESS);
                }
            }

            page_pool_free(pt);
        }
    }

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

    for (address = 0; address < len; address += PAGE_SIZE) {
        k_u64_t virtual_address = vaddr + address;
        k_u64_t physical_address = paddr + address;
        k_u64_t pd_index;
        k_u64_t pt_index;
        k_u64_t *pt;

        if (virtual_address < PORT_X86_64_USER_BASE || virtual_address >= PORT_X86_64_USER_LIMIT) {
            return -1;
        }

        pd_index = (virtual_address >> 21) & (TABLE_ENTRIES - 1u);
        pt_index = (virtual_address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
        pt = space_pt(cr3, pd_index);

        if (pt == NULL) {
            return -1;
        }

        pt[pt_index] = (physical_address & ~(k_u64_t)(PAGE_SIZE - 1u)) | entry_flags;
    }

    return 0;
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

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return 0;
    }

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);

    if (pt == NULL) {
        return 0;
    }

    entry = pt[pt_index];

    /* Only a page of the program that is there, open to ring 3 and not
     * writable is this case; a kernel page of rodata is a bug of the kernel
     * and the machine stops there, as it always did. */
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_WRITABLE)) != (PTE_PRESENT | PTE_USER)) {
        return 0;
    }

    old_frame = entry & PTE_ADDRESS;
    new_frame = port_x86_64_frame_alloc();
    if (new_frame == 0) {
        return 0;
    }

    memcpy((void *)(uintptr_t)new_frame, (const void *)(uintptr_t)old_frame, PAGE_SIZE);

    pt[pt_index] = new_frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    port_x86_64_frame_release(old_frame);

    return 1;
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

    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return 0;
    }

    pd_index = (vaddr >> 21) & (TABLE_ENTRIES - 1u);
    pt_index = (vaddr >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    pt = space_pt(cr3, pd_index);

    if (pt == NULL) {
        return 0;
    }

    entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0 || (entry & PTE_COW) == 0) {
        return 0;
    }

    old_frame = entry & PTE_ADDRESS;
    new_frame = port_x86_64_frame_alloc();
    if (new_frame == 0) {
        return 0;
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

    port_x86_64_frame_release(old_frame);

    return 1;
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
