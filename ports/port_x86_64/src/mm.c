/*
 * Micro OS — the x86-64 port: the pages a program maps.
 *
 * The window of the programs (paging.c) is 16 MiB of 4 KiB pages backed by the
 * arena at 64 MiB. This file is the contract of kernel/port/mm.h over it: the
 * kernel asks for the page behind an address when the program faults on it,
 * fills it through the address of the kernel that comes back, and the page is
 * the frame of the arena that the program sees through its own tables.
 *
 * The space is CR3: a program that is being placed by `execve` has no task
 * yet, and the tables are the only thing that names its memory. Every call
 * refuses an address outside the window and a space that is zero, so a
 * mistake of the kernel is an error of the call and not a page table that
 * points at the kernel.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/port/mm.h"
#include "kernel/port_task.h"
#include "port_x86_64_task.h"

#define PAGE_SIZE 4096u

static k_u32_t paging_flags(k_u32_t prot)
{
    k_u32_t flags = 0;

    if ((prot & K_PORT_MM_WRITE) != 0) {
        flags |= PORT_X86_64_PAGE_WRITE;
    }

    if ((prot & K_PORT_MM_EXEC) != 0) {
        flags |= PORT_X86_64_PAGE_EXEC;
    }

    return flags;
}

bool k_port_mm_supported(void)
{
    return true;
}

k_u64_t k_port_mm_base(void)
{
    return (k_u64_t)PORT_X86_64_USER_BASE;
}

k_u64_t k_port_mm_limit(void)
{
    return (k_u64_t)PORT_X86_64_USER_LIMIT;
}

k_u64_t k_port_mm_top(void)
{
    return (k_u64_t)PORT_X86_64_USER_TOP;
}

k_u64_t k_port_mm_current(void)
{
    return port_x86_64_task_space();
}

void *k_port_mm_page(k_u64_t space, k_u64_t vaddr, k_u32_t prot)
{
    if (space == 0) {
        return NULL;
    }

    return port_x86_64_paging_map_page(space, vaddr, paging_flags(prot));
}

void *k_port_mm_writable(k_u64_t space, k_u64_t vaddr)
{
    if (space == 0 || !port_x86_64_paging_page_writable(space, vaddr)) {
        return NULL;
    }

    return port_x86_64_paging_phys(space, vaddr);
}

void *k_port_mm_private(k_u64_t space, k_u64_t vaddr)
{
    if (space == 0 || !port_x86_64_paging_page_private(space, vaddr)) {
        return NULL;
    }

    return port_x86_64_paging_phys(space, vaddr);
}

bool k_port_mm_present(k_u64_t space, k_u64_t vaddr)
{
    if (space == 0) {
        return false;
    }

    return port_x86_64_paging_phys(space, vaddr) != NULL;
}

void *k_port_mm_phys(k_u64_t space, k_u64_t vaddr)
{
    if (space == 0) {
        return NULL;
    }

    return port_x86_64_paging_phys(space, vaddr);
}

void k_port_mm_unmap(k_u64_t space, k_u64_t vaddr, k_size_t len)
{
    k_size_t at;

    if (space == 0) {
        return;
    }

    for (at = 0; at < len; at += PAGE_SIZE) {
        port_x86_64_paging_unmap_page(space, vaddr + at);
    }
}
