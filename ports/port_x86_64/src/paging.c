/*
 * Micro OS — the page tables of the x86-64 port.
 *
 * Two maps live here. The kernel's is the one the loader used to make: the
 * first gigabyte of the machine, identity mapped in 2 MiB pages and reachable
 * from ring 0 only. Every program gets a set of its own whose first gigabyte
 * is a copy of that map, except for the window of the programs: those 16 MiB
 * are 4 KiB pages with the user bit, and the physical pages behind them come
 * from an arena outside the heap. That is what makes a program a user of the
 * machine and not a function of the kernel: the address it computes is not
 * the address the page lives at, and nothing of the kernel is under it.
 *
 * One program runs at a time on this port, so the tables of the program in
 * flight are one set. They are still built and destroyed per program: the
 * pages - the PML4, the PDPT, the PD, the PTE tables - come from a pool in
 * the image, and the next program gets freshly zeroed ones.
 */
#include "port_x86_64.h"

#include <string.h>

#define PAGE_SIZE 4096u
#define PAGE_SHIFT 12u
#define TABLE_ENTRIES 512u

#define PTE_PRESENT 0x001u
#define PTE_WRITABLE 0x002u
#define PTE_USER 0x004u
#define PTE_PAGE_SIZE 0x080u /* in a PD entry: a 2 MiB page */

#define USER_PD_FIRST ((PORT_X86_64_USER_BASE >> 21) & (TABLE_ENTRIES - 1u))
#define USER_PD_COUNT ((PORT_X86_64_USER_LIMIT - PORT_X86_64_USER_BASE) >> 21)
#define USER_PHYS_LIMIT (PORT_X86_64_USER_PHYS + (PORT_X86_64_USER_LIMIT - PORT_X86_64_USER_BASE))

/*
 * The page tables of the programs. A program needs twelve pages - a PML4, a
 * PDPT, a PD, a PTE table for the low memory and eight for the window - so
 * the pool holds enough for a handful
 * before an exec starts saying "no memory". It is in the image and not in the
 * heap because the pool has to outlive the allocator's bookkeeping and be
 * page aligned by the linker, not by a request.
 */
#define PAGE_POOL_PAGES 64u
#define PAGE_POOL_WORDS (PAGE_POOL_PAGES / 64u)

static k_u64_t g_kernel_pml4[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pdpt[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pd[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static k_u8_t g_page_pool[PAGE_POOL_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_page_bitmap[PAGE_POOL_WORDS];

/* The set of the program in flight; port_exec.c asks for one at a time. */
static k_u64_t *g_space_pml4;
static k_u64_t *g_space_pdpt;
static k_u64_t *g_space_pd;
static k_u64_t *g_space_low_pt;
static k_u64_t *g_space_pt[USER_PD_COUNT];

static k_u64_t page_phys(const void *page)
{
    /* The identity map makes the virtual and the physical address the same
     * number for everything the kernel owns, page tables included. */
    return (k_u64_t)(uintptr_t)page;
}

static void *page_pool_alloc(void)
{
    k_u32_t word;
    k_u32_t bit;

    for (word = 0; word < PAGE_POOL_WORDS; word++) {
        if (g_page_bitmap[word] == ~(k_u64_t)0) {
            continue;
        }

        for (bit = 0; bit < 64u; bit++) {
            k_u64_t mask = (k_u64_t)1u << bit;

            if ((g_page_bitmap[word] & mask) == 0) {
                k_u8_t *page;

                g_page_bitmap[word] |= mask;
                page = g_page_pool + ((k_size_t)word * 64u + bit) * PAGE_SIZE;
                memset(page, 0, PAGE_SIZE);
                return page;
            }
        }
    }

    return NULL;
}

static void page_pool_free(void *page)
{
    k_u8_t *base = (k_u8_t *)page;
    k_size_t index;
    k_u32_t word;
    k_u32_t bit;

    if (base < g_page_pool || base >= g_page_pool + sizeof(g_page_pool)) {
        return;
    }

    index = (k_size_t)(base - g_page_pool) / PAGE_SIZE;
    word = (k_u32_t)(index / 64u);
    bit = (k_u32_t)(index % 64u);
    g_page_bitmap[word] &= ~((k_u64_t)1u << bit);
}

static void write_cr3(k_u64_t cr3)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

void port_x86_64_paging_init(void)
{
    k_u32_t index;

    /*
     * The identity map of the first gigabyte. The kernel image, the heap and
     * the physical arena of the programs are all inside it; a 2 MiB page has
     * no user bit, so ring 3 sees none of them even while its own CR3 is the
     * one in the register.
     */
    for (index = 0; index < TABLE_ENTRIES; index++) {
        g_kernel_pd[index] = ((k_u64_t)index << 21) | PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    g_kernel_pdpt[0] = page_phys(g_kernel_pd) | PTE_PRESENT | PTE_WRITABLE;
    g_kernel_pml4[0] = page_phys(g_kernel_pdpt) | PTE_PRESENT | PTE_WRITABLE;

    write_cr3(page_phys(g_kernel_pml4));
}

k_u64_t port_x86_64_kernel_cr3(void)
{
    return page_phys(g_kernel_pml4);
}

k_u64_t port_x86_64_paging_space_create(void)
{
    k_u32_t index;

    if (g_space_pml4 != NULL) {
        /* One program at a time: the caller has to finish the first one. */
        return 0;
    }

    g_space_pml4 = page_pool_alloc();
    g_space_pdpt = page_pool_alloc();
    g_space_pd = page_pool_alloc();
    g_space_low_pt = page_pool_alloc();

    if (g_space_pml4 == NULL || g_space_pdpt == NULL || g_space_pd == NULL ||
        g_space_low_pt == NULL) {
        goto fail;
    }

    /*
     * The kernel map is copied, not shared: the program's PD replaces the
     * entries of the window and the kernel's table must keep the 2 MiB pages
     * it was built with.
     */
    memcpy(g_space_pd, g_kernel_pd, sizeof(g_kernel_pd));

    /*
     * The first 2 MiB stay reachable for the kernel - the console writes the
     * text memory at 0xB8000 during a system call of the program - but the
     * zero page itself is not a page: a null pointer of a program faults, and
     * the rest of the low memory is supervisor-only, so ring 3 cannot read it
     * even though it is mapped.
     */
    for (index = 0; index < TABLE_ENTRIES; index++) {
        if (index == 0) {
            g_space_low_pt[index] = 0;
        } else {
            g_space_low_pt[index] = ((k_u64_t)index << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITABLE;
        }
    }

    g_space_pd[0] = page_phys(g_space_low_pt) | PTE_PRESENT | PTE_WRITABLE;

    for (index = 0; index < USER_PD_COUNT; index++) {
        g_space_pt[index] = page_pool_alloc();

        if (g_space_pt[index] == NULL) {
            goto fail;
        }

        g_space_pd[USER_PD_FIRST + index] =
            page_phys(g_space_pt[index]) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    /*
     * The user bit is checked at every level of the walk, not only at the
     * page: the two upper entries of the program's map open the door for the
     * window, and the kernel entries of the PD keep it shut for everything
     * else. A copy of the kernel's own upper entries would refuse ring 3
     * before it ever looked at the leaf.
     */
    g_space_pdpt[0] = page_phys(g_space_pd) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    g_space_pml4[0] = page_phys(g_space_pdpt) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    return page_phys(g_space_pml4);

fail:
    for (index = 0; index < USER_PD_COUNT; index++) {
        if (g_space_pt[index] != NULL) {
            page_pool_free(g_space_pt[index]);
            g_space_pt[index] = NULL;
        }
    }

    if (g_space_pml4 != NULL) {
        page_pool_free(g_space_pml4);
        g_space_pml4 = NULL;
    }
    if (g_space_pdpt != NULL) {
        page_pool_free(g_space_pdpt);
        g_space_pdpt = NULL;
    }
    if (g_space_low_pt != NULL) {
        page_pool_free(g_space_low_pt);
        g_space_low_pt = NULL;
    }

    if (g_space_pd != NULL) {
        page_pool_free(g_space_pd);
        g_space_pd = NULL;
    }

    return 0;
}

void port_x86_64_paging_space_destroy(k_u64_t cr3)
{
    k_u32_t index;

    if (g_space_pml4 == NULL || cr3 != page_phys(g_space_pml4)) {
        return;
    }

    for (index = 0; index < USER_PD_COUNT; index++) {
        if (g_space_pt[index] != NULL) {
            page_pool_free(g_space_pt[index]);
            g_space_pt[index] = NULL;
        }
    }

    if (g_space_low_pt != NULL) {
        page_pool_free(g_space_low_pt);
        g_space_low_pt = NULL;
    }

    page_pool_free(g_space_pd);
    page_pool_free(g_space_pdpt);
    page_pool_free(g_space_pml4);

    g_space_pd = NULL;
    g_space_pdpt = NULL;
    g_space_pml4 = NULL;
}

int port_x86_64_paging_map_user(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    k_u64_t address;

    if (g_space_pml4 == NULL || (vaddr & (PAGE_SIZE - 1u)) != 0 ||
        (paddr & (PAGE_SIZE - 1u)) != 0 || (len & (PAGE_SIZE - 1u)) != 0) {
        return -1;
    }

    for (address = 0; address < len; address += PAGE_SIZE) {
        k_u64_t virtual_address = vaddr + address;
        k_u64_t physical_address = paddr + address;
        k_u64_t pd_index;
        k_u64_t pt_index;

        if (virtual_address < PORT_X86_64_USER_BASE || virtual_address >= PORT_X86_64_USER_LIMIT ||
            physical_address < PORT_X86_64_USER_PHYS || physical_address >= USER_PHYS_LIMIT) {
            return -1;
        }

        pd_index = (virtual_address >> 21) & (TABLE_ENTRIES - 1u);
        pt_index = (virtual_address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);

        if (g_space_pt[pd_index - USER_PD_FIRST] == NULL) {
            return -1;
        }

        g_space_pt[pd_index - USER_PD_FIRST][pt_index] =
            (physical_address & ~(k_u64_t)(PAGE_SIZE - 1u)) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    return 0;
}

void *port_x86_64_paging_phys(k_u64_t vaddr)
{
    if (vaddr < PORT_X86_64_USER_BASE || vaddr >= PORT_X86_64_USER_LIMIT) {
        return NULL;
    }

    return (void *)(uintptr_t)(PORT_X86_64_USER_PHYS + (vaddr - PORT_X86_64_USER_BASE));
}

void port_x86_64_paging_switch(k_u64_t cr3)
{
    write_cr3(cr3);
}
