/*
 * Micro OS — the page tables of the x86-64 port.
 *
 * Two maps live here. The kernel's is the one the loader used to make: the
 * first gigabyte of the machine, identity mapped in 2 MiB pages and reachable
 * from ring 0 only. Every task with a program gets a set of its own whose
 * first gigabyte is a copy of that map, except for the window of the programs:
 * those 16 MiB are 4 KiB pages with the user bit, and the physical pages
 * behind them come from an arena outside the heap. That is what makes a
 * program a user of the machine and not a function of the kernel: the address
 * it computes is not the address the page lives at, and nothing of the kernel
 * is under it.
 *
 * A set of tables is one address space and it belongs to a task (ADR 0007,
 * K4). The tables themselves come from a pool of pages in the image; the
 * physical pages of the window come from a frame allocator over the arena.
 * A fork does not copy a byte of the parent: it copies the tables, points
 * both copies at the same frames and takes the write bit away. The fault of
 * the first write makes a private copy of that one page, which is the whole
 * of copy on write.
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

/*
 * A page that both the parent and the child of a fork may still read and
 * neither may write: the entry of the PTE that says "copy it on the first
 * write". The bit is free for the operating system, and the processor
 * ignores it.
 */
#define PTE_COW 0x200u

#define PTE_ADDRESS 0x000FFFFFFFFFF000ull

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
static k_u64_t g_kernel_pdpt[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_kernel_pd[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static k_u8_t g_page_pool[PAGE_POOL_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static k_u64_t g_page_bitmap[PAGE_POOL_WORDS];

/*
 * One reference per page of the arena. A fork makes two spaces point at the
 * same frame, so a space that ends may not free a page the other one still
 * reads; the count is what tells the last one from the first one.
 */
static k_u16_t g_frame_ref[USER_FRAME_COUNT];

static k_u64_t page_phys(const void *page)
{
    /* The identity map makes the virtual and the physical address the same
     * number for everything the kernel owns, page tables included. */
    return (k_u64_t)(uintptr_t)page;
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
    k_u8_t *base = (k_u8_t *)page;
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

void port_x86_64_paging_init(void)
{
    k_u32_t index;
    k_u64_t cr0;

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

    /*
     * The kernel has to honour the write bit of its own tables. A system
     * call writes the memory of the program (the status of waitpid, the
     * buffer of a read), and a page that a fork made read-only has to fault
     * in ring 0 as well, or the kernel would write into the frame the child
     * is still reading. With WP set the page fault handler sees the write
     * and copies the page, exactly as it does for a write of the program.
     */
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
     * The kernel map is copied, not shared: the program's PD replaces the
     * entries of the window and the kernel's table must keep the 2 MiB pages
     * it was built with.
     */
    memcpy(pd, g_kernel_pd, sizeof(g_kernel_pd));

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
            low_pt[index] = ((k_u64_t)index << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITABLE;
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

            frame_ref(entry & PTE_ADDRESS);

            /* Both copies read the frame and neither may write it until the
             * fault of the first write gives one of them a page of its own. */
            child_pt[pt_index] = (entry & ~PTE_WRITABLE) | PTE_COW;
            parent_pt[pt_index] = (entry & ~PTE_WRITABLE) | PTE_COW;

            /* The parent wrote through this entry before the fork. */
            invlpg(address);
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

int port_x86_64_paging_map_user(k_u64_t cr3, k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    k_u64_t address;

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

        pt[pt_index] =
            (physical_address & ~(k_u64_t)(PAGE_SIZE - 1u)) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    return 0;
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

    pt[pt_index] = new_frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    invlpg(vaddr & ~(k_u64_t)(PAGE_SIZE - 1u));

    port_x86_64_frame_release(old_frame);

    return 1;
}

void port_x86_64_paging_switch(k_u64_t cr3)
{
    write_cr3(cr3);
}
