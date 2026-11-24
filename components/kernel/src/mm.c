/*
 * Micro OS — the memory a program maps.
 *
 * A program of this system may ask for memory in two ways: `brk` moves the
 * end of its heap, and `mmap` names a range that is backed by a file or by
 * zeros. Neither of them reads anything when the call is made. The range is a
 * record here - the region - and the page behind an address is made, filled
 * and mapped the moment the program touches it: the fault of the page comes
 * back into this file, the bytes of the file are read through the VFS, and
 * the port maps the frame. A program that maps a file of 64 MiB and reads one
 * page of it costs one page, not 64 MiB.
 *
 * A region is one entry of a list of the address space, in the order the
 * program made them, and the list is the whole of what this system knows
 * about the memory of a program: there is no page table here, because the
 * tables belong to the port. The flags are the two of POSIX that this system
 * has - a shared mapping writes through to the file, a private one copies the
 * page on the first write - and the protection is the three bits of the ABI.
 *
 * The heap is a region too, with no file behind it: `brk` only moves its end
 * and the pages below it are made as the program writes into them. The image
 * of a program is three regions at the base of the window, and a program that
 * writes its data page gets a copy of that page, which is what `execve` of a
 * file means when the file is not read into a buffer first.
 */
#include "kernel/mm.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/mm.h"
#include "kernel/vfs.h"

/*
 * The spaces of the system. A space belongs to a program and is named by the
 * handle of the port (CR3 on x86-64); the table is the size of the table of
 * the tasks because a program is a task. An entry is given back when the task
 * leaves, so the handle of a finished program is not reused by accident.
 */
struct k_mm_space {
    bool used;
    k_u64_t handle;
    struct k_mm_region *regions;
    k_u64_t brk;      /* the current end of the heap */
    k_u64_t brk_base; /* where the heap began, the end of the image */
};

static struct k_mm_space g_spaces[TASK_MAX_TASKS];

/* The space of the caller, zero when the machine has no page tables or the
 * caller is a task of the kernel. */
k_mm_space_t k_mm_current(void)
{
    return k_port_mm_supported() ? k_port_mm_current() : 0;
}

/*
 * A page is code or it is data (ADR 0052). The rule is the one the tables of
 * every program already carry - a page of the text is executable and not
 * writable, every data page, the stack, the arena and the heap are writable
 * and NX - and here it is stated once, where a mapping is asked for, so that
 * a program cannot ask the kernel for the one page the processor refuses to
 * build honestly.
 */
bool k_mm_prot_allowed(k_u32_t prot)
{
    return (prot & (k_u32_t)(K_MM_PROT_WRITE | K_MM_PROT_EXEC)) !=
           (k_u32_t)(K_MM_PROT_WRITE | K_MM_PROT_EXEC);
}

static struct k_mm_space *space_find(k_mm_space_t handle)
{
    k_size_t i;

    if (handle == 0) {
        return NULL;
    }

    for (i = 0; i < K_ARRAY_SIZE(g_spaces); i++) {
        if (g_spaces[i].used && g_spaces[i].handle == handle) {
            return &g_spaces[i];
        }
    }

    return NULL;
}

static struct k_mm_space *space_get(k_mm_space_t handle)
{
    struct k_mm_space *space = space_find(handle);
    k_size_t i;

    if (space != NULL || handle == 0) {
        return space;
    }

    for (i = 0; i < K_ARRAY_SIZE(g_spaces); i++) {
        if (!g_spaces[i].used) {
            g_spaces[i].used = true;
            g_spaces[i].handle = handle;
            g_spaces[i].regions = NULL;
            g_spaces[i].brk = 0;
            g_spaces[i].brk_base = 0;
            return &g_spaces[i];
        }
    }

    return NULL;
}

static k_u64_t page_down(k_u64_t address)
{
    return address & ~(k_u64_t)(K_MM_PAGE_SIZE - 1u);
}

static k_u64_t page_up(k_u64_t address)
{
    return page_down(address + (k_u64_t)K_MM_PAGE_SIZE - 1u);
}

/* The region that holds `address`, or NULL. */
static struct k_mm_region *region_find(struct k_mm_space *space, k_u64_t address)
{
    struct k_mm_region *region;

    for (region = space->regions; region != NULL; region = region->next) {
        if (address >= region->start && address < region->start + region->len) {
            return region;
        }
    }

    return NULL;
}

static struct k_mm_region *region_new(void)
{
    struct k_mm_region *region =
        (struct k_mm_region *)kmm_alloc(sizeof(*region), KMM_PRIORITY_SYSTEM);

    if (region != NULL) {
        memset(region, 0, sizeof(*region));
    }

    return region;
}

static void region_free(struct k_mm_region *region)
{
    if (region == NULL) {
        return;
    }

    if (region->file_open) {
        (void)vfs_close(&region->file);
    }

    if (region->dirty != NULL) {
        kmm_free(region->dirty);
    }

    kmm_free(region);
}

/* --- the pages --- */

/*
 * Fill one page of a region from its file. The page is the memory of the
 * kernel that the port handed out, so the read is an ordinary one; a page
 * past the end of the file is zeros, which is what a mapping of a file that
 * is shorter than its region reads.
 */
static int region_fill(struct k_mm_region *region, k_u64_t vaddr, void *page)
{
    k_u64_t offset;
    int got = 0;

    memset(page, 0, K_MM_PAGE_SIZE);

    if (!region->file_open) {
        return 0;
    }

    offset = region->offset + (vaddr - region->start);

    if (offset >= region->file.size) {
        return 0;
    }

    if (region->file.offset != offset) {
        region->file.offset = offset;
    }

    got = vfs_read(&region->file, page, K_MM_PAGE_SIZE);
    if (got < 0) {
        return got;
    }

    return 0;
}

static bool region_is_shared(const struct k_mm_region *region)
{
    return (region->flags & K_MM_SHARED) != 0;
}

static bool region_is_writable(const struct k_mm_region *region)
{
    return (region->prot & K_MM_PROT_WRITE) != 0;
}

/*
 * The bytes of the anonymous mappings of a space, which is what the budget of
 * the task counts (ADR 0052). The answer is read from the regions of the list
 * and not kept in a counter of its own: a counter would be a second truth
 * that `munmap`, a fork and the end of the space each have to update, and the
 * list is the hand of records a program can make. A region leaves the list
 * when it is unmapped, so the bytes it counted go with it.
 */
static k_u64_t space_mapped(const struct k_mm_space *space)
{
    k_u64_t total = 0;
    const struct k_mm_region *region;

    for (region = space->regions; region != NULL; region = region->next) {
        if ((region->flags & K_MM_CHARGED) != 0) {
            total += region->len;
        }
    }

    return total;
}

/* The bit of a page in the map of the pages that were written. It is the
 * record of the first store of a page (the write fault), not the list of the
 * pages to write back: `region_writeback` names the pages that are present,
 * because a page that is already writable takes its later stores without a
 * fault. */
static void region_mark_dirty(struct k_mm_region *region, k_u64_t vaddr)
{
    k_size_t index;

    if (region->dirty == NULL) {
        return;
    }

    index = (k_size_t)((vaddr - region->start) / K_MM_PAGE_SIZE);

    region->dirty[index / 8u] |= (k_u8_t)(1u << (index % 8u));
}

/*
 * Write the pages of a shared mapping back to its file.
 *
 * A page that is present is a page the program has touched, and it is the one
 * to write: the bits of `dirty` are a hint of the first write of a page and
 * not the truth about the others - a page mapped writable by its first fault
 * takes every later store without a fault, so a bit that only a fault sets
 * would name the first frame of a program and no other (the window of the
 * desktop is a buffer that changes a hundred times). The write of a page that
 * did not change is a page of the file that the file already has, and the
 * cost of it is the price of a mapping that is a mapping.
 */
static int region_writeback(struct k_mm_space *space, struct k_mm_region *region)
{
    k_size_t pages = (k_size_t)(region->len / K_MM_PAGE_SIZE);
    k_size_t index;

    if (!region->file_open || !region_is_shared(region)) {
        return 0;
    }

    for (index = 0; index < pages; index++) {
        k_u64_t vaddr = region->start + (k_u64_t)index * K_MM_PAGE_SIZE;
        k_u64_t offset = region->offset + (k_u64_t)index * K_MM_PAGE_SIZE;
        void *page;
        int rc;

        page = k_port_mm_phys(space->handle, vaddr);

        if (page == NULL) {
            /* The page was never touched since the mapping was made: the file
             * has the bytes already. */
            region->dirty[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));
            continue;
        }

        if (region->file.offset != offset) {
            region->file.offset = offset;
        }

        rc = vfs_write(&region->file, page, K_MM_PAGE_SIZE);
        if (rc < 0) {
            return rc;
        }

        region->dirty[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));
    }

    return 0;
}

/*
 * The fault of a page of a program. The answer is 0 when the instruction that
 * faulted may be run again and -1 when the address is not memory the program
 * asked for, which is the fault of the program and not of the machine.
 */
int k_mm_fault(k_mm_space_t handle, k_u64_t address, bool write)
{
    struct k_mm_space *space = space_find(handle);
    struct k_mm_region *region;
    k_u64_t vaddr = page_down(address);
    k_u32_t prot;
    void *page;

    if (space == NULL) {
        return -1;
    }

    region = region_find(space, vaddr);
    if (region == NULL) {
        return -1;
    }

    if (write && !region_is_writable(region)) {
        return -1;
    }

    if (k_port_mm_present(handle, vaddr)) {
        if (!write) {
            /* A page that is there and faulted on a read is a fetch of a page
             * that is not code, or a page that the tables refuse: the program
             * is wrong, not the mapping. */
            return -1;
        }

        if (region_is_shared(region)) {
            page = k_port_mm_writable(handle, vaddr);
            if (page == NULL) {
                return -1;
            }

            region_mark_dirty(region, vaddr);
            return 0;
        }

        page = k_port_mm_private(handle, vaddr);
        return page != NULL ? 0 : -1;
    }

    /*
     * A page that is not there yet. The frame is fresh, so it belongs to this
     * space alone: a private page that is written is mapped writable at once
     * and one that is only read stays read-only until the program writes it.
     * A shared page is the frame the file was read into, and a write of it is
     * a write of the file - the page is marked dirty for `msync`.
     */
    prot = region->prot & (K_MM_PROT_READ | K_MM_PROT_EXEC);

    if (write) {
        prot |= K_MM_PROT_WRITE;
    }

    page = k_port_mm_page(handle, vaddr, prot);
    if (page == NULL) {
        return -1;
    }

    if (region_fill(region, vaddr, page) != 0) {
        return -1;
    }

    if (write && region_is_shared(region)) {
        region_mark_dirty(region, vaddr);
    }

    return 0;
}

/* Make the pages of a range present and filled, which is what a caller that
 * reads or writes the memory of a program from the kernel needs. */
int k_mm_touch(k_mm_space_t handle, k_u64_t address, k_size_t len, bool write)
{
    k_u64_t at;

    if (len == 0) {
        return 0;
    }

    for (at = page_down(address); at < address + len; at += K_MM_PAGE_SIZE) {
        if (k_port_mm_present(handle, at)) {
            continue;
        }

        if (k_mm_fault(handle, at, write) != 0) {
            return KERR_FAULT;
        }
    }

    return 0;
}

/* --- the regions --- */

/*
 * The first free range of `len` bytes in [low, high), or 0. The list of the
 * regions is in the order of the addresses, so a walk that runs into one
 * jumps past it and looks again: a mapping of a program is a handful of
 * regions and a first fit is cheaper than a tree.
 */
static k_u64_t region_gap(struct k_mm_space *space, k_u64_t low, k_u64_t high, k_size_t len)
{
    k_u64_t candidate = page_up(low);

    for (;;) {
        struct k_mm_region *region;
        bool moved = false;

        for (region = space->regions; region != NULL; region = region->next) {
            if (region->start + region->len <= candidate) {
                continue;
            }

            if (region->start >= candidate + (k_u64_t)len) {
                break;
            }

            candidate = page_up(region->start + region->len);
            moved = true;
            break;
        }

        if (!moved) {
            return (candidate + (k_u64_t)len <= high) ? candidate : 0;
        }

        if (candidate + (k_u64_t)len > high) {
            return 0;
        }
    }
}

static void region_insert(struct k_mm_space *space, struct k_mm_region *region)
{
    struct k_mm_region **slot = &space->regions;

    /* The list is kept in the order of the addresses: the first fit of a new
     * mapping and the walk of a fault both read it. */
    while (*slot != NULL && (*slot)->start < region->start) {
        slot = &(*slot)->next;
    }

    region->next = *slot;
    *slot = region;
}

/* Open the file of a region and give it the map of the dirty pages. */
static int region_attach_file(struct k_mm_region *region, const char *path, k_u32_t flags)
{
    int open_flags = VFS_O_RDONLY;
    int rc;

    if ((flags & K_MM_SHARED) != 0 && (region->prot & K_MM_PROT_WRITE) != 0) {
        open_flags = VFS_O_RDWR;
    }

    rc = vfs_open(path, open_flags, &region->file, region->priv);
    if (rc != 0) {
        return rc;
    }

    region->file_open = true;

    if ((flags & K_MM_SHARED) != 0) {
        k_size_t pages = (k_size_t)(region->len / K_MM_PAGE_SIZE);

        region->dirty = (k_u8_t *)kmm_alloc((pages + 7u) / 8u, KMM_PRIORITY_SYSTEM);
        if (region->dirty == NULL) {
            (void)vfs_close(&region->file);
            region->file_open = false;
            return KERR_NOMEM;
        }

        memset(region->dirty, 0, (pages + 7u) / 8u);
    }

    return 0;
}

int k_mm_map(k_mm_space_t handle, k_u64_t address, k_size_t len, k_u32_t prot, k_u32_t flags,
             const char *path, k_u64_t offset, k_u64_t *out)
{
    struct k_mm_space *space = space_get(handle);
    struct k_mm_region *region;
    k_u64_t start;
    k_u64_t top;
    int rc;

    if (space == NULL || len == 0 || out == NULL) {
        return KERR_INVAL;
    }

    if ((prot & ~(k_u32_t)(K_MM_PROT_READ | K_MM_PROT_WRITE | K_MM_PROT_EXEC)) != 0 ||
        (flags & ~(k_u32_t)(K_MM_SHARED | K_MM_PRIVATE | K_MM_ANONYMOUS)) != 0 ||
        ((flags & K_MM_SHARED) != 0 && (flags & K_MM_PRIVATE) != 0) ||
        (offset % K_MM_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    /*
     * The policy of the page (ADR 0052): a program may not ask for a page it
     * may write and run at once. The refusal is the call and not a page fault
     * later, so a program that asks for a code it built itself is told EINVAL
     * and keeps running; the tables of the port never carry such a page.
     */
    if (!k_mm_prot_allowed(prot)) {
        return KERR_INVAL;
    }

    if ((flags & K_MM_ANONYMOUS) == 0 && path == NULL) {
        return KERR_INVAL;
    }

    if ((flags & K_MM_ANONYMOUS) != 0 && path != NULL) {
        return KERR_INVAL;
    }

    /* A length that wraps the address space is not a length. */
    if (address + (k_u64_t)len < address) {
        return KERR_INVAL;
    }

    top = k_port_mm_top();

    if (address != 0) {
        /* The address is a hint: it is taken when the range is free, and the
         * call places the mapping somewhere else when it is not. */
        start = page_down(address);

        if (start >= k_port_mm_base() && start + (k_u64_t)len <= top &&
            region_gap(space, start, start + (k_u64_t)len, len) == start) {
            /* free */
        } else {
            start = 0;
        }
    } else {
        start = 0;
    }

    if (start == 0) {
        k_u64_t low = (space->brk != 0) ? space->brk : k_port_mm_base();

        start = region_gap(space, low, top, len);
        if (start == 0) {
            return KERR_NOMEM;
        }
    }

    region = region_new();
    if (region == NULL) {
        return KERR_NOMEM;
    }

    region->start = start;
    region->len = (k_u64_t)len;
    if (path != NULL) {
        k_size_t i = 0;

        while (i + 1 < sizeof(region->path) && path[i] != '\0') {
            region->path[i] = path[i];
            i++;
        }

        region->path[i] = '\0';
    }
    region->offset = offset;
    region->prot = prot;
    region->flags = (flags & K_MM_ANONYMOUS) != 0
                        ? K_MM_ANONYMOUS
                        : ((flags & K_MM_SHARED) != 0 ? K_MM_SHARED : K_MM_PRIVATE);

    /*
     * An anonymous mapping is memory the program owns and not a page of a
     * file, so it is bounded by the budget of the task that makes it, the
     * same KSYS_RLIMIT_MEMORY the heap is counted against (ADR 0052). The
     * call is the one of the program: a space that the kernel is filling for
     * another task (execve places the image of a program in a space that is
     * not the space of the caller) is not charged to the caller, which would
     * put a program's bss on the shell's budget.
     */
    if (region->flags == K_MM_ANONYMOUS && handle == k_port_mm_current()) {
        struct task *self = task_self();

        if (self != NULL && !task_memory_fits(self, space_mapped(space) + (k_u64_t)len)) {
            region_free(region);
            return KERR_NOMEM;
        }

        region->flags |= K_MM_CHARGED;
    }

    if ((region->flags & K_MM_ANONYMOUS) == 0) {
        rc = region_attach_file(region, path, region->flags);
        if (rc != 0) {
            region_free(region);
            return rc;
        }
    }

    region_insert(space, region);
    *out = start;

    return 0;
}

int k_mm_unmap(k_mm_space_t handle, k_u64_t address, k_size_t len)
{
    struct k_mm_space *space = space_find(handle);
    struct k_mm_region **slot;
    k_u64_t start;
    k_u64_t end;
    k_size_t unmapped = 0;

    if (space == NULL || len == 0 || (address % K_MM_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    start = address;
    end = address + (k_u64_t)len;

    if (end < start) {
        return KERR_INVAL;
    }

    /*
     * Every mapping of the range has to be wholly inside it, and the answer
     * is EINVAL before a page is written back or given up: a refusal that has
     * already written half of the range to its file is not a refusal.
     */
    for (struct k_mm_region *region = space->regions; region != NULL; region = region->next) {
        if (region->start < end && region->start + region->len > start) {
            if (region->start < start || region->start + region->len > end) {
                return KERR_INVAL;
            }
        }
    }

    /* A shared mapping goes back to its file before the frames are gone. */
    for (struct k_mm_region *region = space->regions; region != NULL; region = region->next) {
        if (region->start < end && region->start + region->len > start) {
            int rc = region_writeback(space, region);

            if (rc != 0) {
                return rc;
            }
        }
    }

    slot = &space->regions;

    while (*slot != NULL) {
        struct k_mm_region *region = *slot;

        if (region->start >= end || region->start + region->len <= start) {
            slot = &region->next;
            continue;
        }

        *slot = region->next;
        k_port_mm_unmap(handle, region->start, (k_size_t)region->len);
        region_free(region);
        unmapped += (k_size_t)region->len;
    }

    if (unmapped == 0) {
        /* POSIX unmaps a range that holds no mapping without an error; the
         * call answers 0 and nothing is done. */
        return 0;
    }

    return 0;
}

int k_mm_msync(k_mm_space_t handle, k_u64_t address, k_size_t len)
{
    struct k_mm_space *space = space_find(handle);
    k_u64_t end;
    int rc = 0;

    if (space == NULL || len == 0) {
        return KERR_INVAL;
    }

    end = address + (k_u64_t)len;
    if (end < address) {
        return KERR_INVAL;
    }

    for (struct k_mm_region *region = space->regions; region != NULL; region = region->next) {
        if (region->start < end && region->start + region->len > address) {
            int result = region_writeback(space, region);

            if (result != 0) {
                rc = result;
            }
        }
    }

    return rc;
}

int k_mm_brk(k_mm_space_t handle, k_u64_t address, k_u64_t *out)
{
    struct k_mm_space *space = space_find(handle);
    struct k_mm_region *heap;
    k_u64_t limit;

    if (space == NULL || space->brk_base == 0) {
        return KERR_NOSYS;
    }

    if (out != NULL) {
        *out = space->brk;
    }

    if (address == 0) {
        return 0;
    }

    /*
     * The heap may not cross a mapping and may not reach the top of the
     * window, where the restorer and the stack of the program are: the answer
     * is ENOMEM and the break stays where it was, which is the refusal of
     * POSIX with a reason a person can read in the shell.
     */
    limit = k_port_mm_top();

    for (struct k_mm_region *region = space->regions; region != NULL; region = region->next) {
        if (region->start >= space->brk && region->start < limit &&
            (region->flags & K_MM_HEAP) == 0) {
            limit = region->start;
        }
    }

    if (address < space->brk_base || address > limit) {
        return KERR_NOMEM;
    }

    if (address < space->brk) {
        /* A heap that shrinks gives the pages above the new end back. */
        k_port_mm_unmap(handle, page_up(address), (k_size_t)(space->brk - page_up(address)));
    }

    heap = NULL;
    for (struct k_mm_region *region = space->regions; region != NULL; region = region->next) {
        if ((region->flags & K_MM_HEAP) != 0) {
            heap = region;
            break;
        }
    }

    if (address > space->brk_base) {
        if (heap == NULL) {
            heap = region_new();

            if (heap == NULL) {
                return KERR_NOMEM;
            }

            heap->start = space->brk_base;
            heap->prot = K_MM_PROT_READ | K_MM_PROT_WRITE;
            heap->flags = K_MM_HEAP | K_MM_ANONYMOUS | K_MM_PRIVATE;
            region_insert(space, heap);
        }

        heap->len = page_up(address) - heap->start;
    } else if (heap != NULL) {
        heap->len = 0;
    }

    space->brk = address;

    if (out != NULL) {
        *out = space->brk;
    }

    return 0;
}

/*
 * The memory of an image: the text (readable, executable, private), the data
 * that follows it (readable, writable, private) and the zeroed part (the
 * bss). The pages come from the file on the fault, so the kernel never holds
 * a copy of the image, and a write of the data of the program copies the page
 * it wrote.
 *
 * The call is the whole of what `execve` needs from this file: the port makes
 * the space, this call records the regions and names where the heap begins.
 */
int k_mm_image(k_mm_space_t handle, const struct k_exec_image *image)
{
    struct k_mm_space *space = space_get(handle);
    k_u64_t text_end;
    k_u64_t data_end;
    k_u64_t bss_end;
    k_u64_t start;
    k_u64_t placed = 0;
    int rc;

    if (space == NULL || image == NULL || image->path == NULL || image->text_size == 0) {
        return KERR_INVAL;
    }

    /*
     * The three regions of an image are pages: the format ends the text on a
     * page, and the data and the bss share the pages that hold their bytes.
     * A region has to begin on a page because the pages of a program are the
     * unit of the map, and the tail of the file's last page reads as zeros
     * (region_fill clears the page before it reads).
     */
    text_end = (k_u64_t)K_EXEC_HEADER_SIZE + (k_u64_t)image->text_size;
    data_end = page_up(text_end + (k_u64_t)image->data_size);
    bss_end = page_up(data_end + (k_u64_t)image->bss_size);

    if (text_end % K_MM_PAGE_SIZE != 0) {
        return KERR_INVAL;
    }

    start = k_port_mm_base();

    if (start + bss_end > k_port_mm_top()) {
        return KERR_NOMEM;
    }

    /* The text: the header and the code, executable and never writable. */
    rc = k_mm_map(handle, start, (k_size_t)text_end, K_MM_PROT_READ | K_MM_PROT_EXEC, K_MM_PRIVATE,
                  image->path, 0, &placed);
    if (rc != 0) {
        return rc;
    }

    space->regions->flags |= K_MM_IMAGE;

    /* The initialised data: readable and writable, and the tail of the last
     * page of the file is zeros. */
    if (data_end > text_end) {
        rc = k_mm_map(handle, start + text_end, (k_size_t)(data_end - text_end),
                      K_MM_PROT_READ | K_MM_PROT_WRITE, K_MM_PRIVATE, image->path, text_end,
                      &placed);
        if (rc != 0) {
            return rc;
        }
    }

    /* The bss: the pages the file does not carry, read as zeros. */
    if (bss_end > data_end) {
        rc = k_mm_map(handle, start + data_end, (k_size_t)(bss_end - data_end),
                      K_MM_PROT_READ | K_MM_PROT_WRITE, K_MM_ANONYMOUS | K_MM_PRIVATE, NULL, 0,
                      &placed);
        if (rc != 0) {
            return rc;
        }
    }

    /* The heap begins on the page after the image, where the port puts the
     * vectors of the entry of the program. */
    space->brk_base = start + bss_end;
    space->brk = space->brk_base;

    return 0;
}

/* Move the beginning of the heap past the vectors the port wrote below the
 * stack. The port calls it once, before the program runs. */
int k_mm_brk_base(k_mm_space_t handle, k_u64_t address)
{
    struct k_mm_space *space = space_find(handle);

    if (space == NULL) {
        return KERR_INVAL;
    }

    if (space->brk_base != 0 && address > space->brk_base) {
        space->brk_base = page_up(address);
        space->brk = space->brk_base;
    }

    return 0;
}

int k_mm_fork(k_mm_space_t parent, k_mm_space_t child)
{
    struct k_mm_space *from = space_find(parent);
    struct k_mm_space *to = space_get(child);
    struct k_mm_region *region;

    if (to == NULL) {
        return KERR_NOMEM;
    }

    if (from == NULL) {
        /* A task of the kernel has no memory of a program to copy. */
        return 0;
    }

    to->brk = from->brk;
    to->brk_base = from->brk_base;

    for (region = from->regions; region != NULL; region = region->next) {
        struct k_mm_region *copy = region_new();
        int rc;

        if (copy == NULL) {
            return KERR_NOMEM;
        }

        *copy = *region;
        copy->next = NULL;
        copy->file_open = false;
        copy->dirty = NULL;

        if (region->file_open) {
            rc = region_attach_file(copy, region->path, copy->flags);

            if (rc != 0) {
                region_free(copy);
                return rc;
            }
        }

        if (region->dirty != NULL) {
            k_size_t pages = (k_size_t)(region->len / K_MM_PAGE_SIZE);

            copy->dirty = (k_u8_t *)kmm_alloc((pages + 7u) / 8u, KMM_PRIORITY_SYSTEM);

            if (copy->dirty == NULL) {
                region_free(copy);
                return KERR_NOMEM;
            }

            memcpy(copy->dirty, region->dirty, (pages + 7u) / 8u);
        }

        region_insert(to, copy);
    }

    return 0;
}

void k_mm_release(k_mm_space_t handle)
{
    struct k_mm_space *space = space_find(handle);

    if (space == NULL) {
        return;
    }

    while (space->regions != NULL) {
        struct k_mm_region *region = space->regions;

        space->regions = region->next;
        region_free(region);
    }

    space->used = false;
    space->handle = 0;
    space->brk = 0;
    space->brk_base = 0;
}
