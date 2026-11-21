/*
 * Micro OS — the x86-64 port: the machine, not the contract.
 *
 * The contract lives in the headers of kernel/port and knows nothing about IN and OUT,
 * COM1 or the 8254. This header is the other half: the few pieces of the PC
 * that the implementation of the contract shares with the entry code, and
 * the numbers the boot of the platform reports.
 *
 * The functions are static inline because they are one instruction each:
 * a call would cost more than the instruction, and the kernel is built
 * without the red zone (an interrupt would overwrite the scratch space below
 * RSP), so the assembly is kept in the header where its assumptions are
 * visible.
 */
#ifndef MICRO_OS_PORT_X86_64_H
#define MICRO_OS_PORT_X86_64_H

#include "kernel/types.h"

/* --- the I/O ports of the PC --------------------------------------------- */

static inline void port_outb(k_u16_t port, k_u8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline k_u8_t port_inb(k_u16_t port)
{
    k_u8_t value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void port_outw(k_u16_t port, k_u16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline k_u16_t port_inw(k_u16_t port)
{
    k_u16_t value;

    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void port_outl(k_u16_t port, k_u32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline k_u32_t port_inl(k_u16_t port)
{
    k_u32_t value;

    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/*
 * The old chips of the PC need a moment between two writes to notice the
 * first one; writing to the unused port 0x80 is the traditional way to spend
 * that moment without a timer.
 */
static inline void port_io_wait(void)
{
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/*
 * The time stamp counter. LFENCE keeps the two reads of a measurement from
 * moving across the instruction, which is what makes the calibration of the
 * PIT below repeatable in a virtual machine.
 */
static inline k_u64_t port_rdtsc(void)
{
    k_u32_t low;
    k_u32_t high;

    __asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((k_u64_t)high << 32) | low;
}

/* --- the instructions of the machine -------------------------------------- */

/*
 * The registers of the machine that are not an I/O port. IA32_FS_BASE
 * (0xC0000100) is where the thread pointer of the library lives: in long
 * mode %fs is an offset from this register and not from a descriptor, and a
 * zero here is a page fault at address zero the first time the library
 * touches its _Thread_local.
 */
static inline void port_wrmsr(k_u32_t msr, k_u64_t value)
{
    k_u32_t low = (k_u32_t)value;
    k_u32_t high = (k_u32_t)(value >> 32);

    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline void port_cli(void)
{
    __asm__ volatile("cli" : : : "memory");
}

static inline void port_sti(void)
{
    __asm__ volatile("sti" : : : "memory");
}

static inline void port_hlt(void)
{
    __asm__ volatile("hlt");
}

/*
 * Save the interrupt flag and disable the interrupts in one step: a critical
 * section has to restore the state it found, not enable the interrupts a
 * nested section may not expect yet.
 */
static inline k_u64_t port_save_flags_and_cli(void)
{
    k_u64_t flags;

    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void port_restore_flags(k_u64_t flags)
{
    __asm__ volatile("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

/* --- the interrupts -------------------------------------------------------- */

/*
 * Own the table of the interrupts: the 32 exceptions, the PIC moved to the
 * vectors 0x20..0x2F, the PIT at 1000 ticks a second, and the interrupts
 * finally on. Called at the end of port_x86_64_init().
 */
void port_x86_64_interrupts_init(void);

/* The ticks of the timer since the table was built; one tick is 1 ms. */
k_u64_t port_x86_64_ticks(void);

/*
 * The C side of the stubs of interrupts.S. An exception says which vector
 * fired, the error code the processor pushed (0 when the vector has none)
 * and the RIP of the moment; both functions print and stop the machine,
 * because neither has anywhere to return to in a kernel this small.
 */
void port_x86_64_unexpected(void);

/*
 * The body of the timer interrupt, called by interrupts.S. `frame` is the
 * frame the stub left on the stack: the registers and the iretq fields of
 * the code the tick interrupted (the layout is the one of
 * PORT_X86_64_SYSCALL_FRAME_SIZE, with the order of the interrupt stub).
 */
void port_x86_64_irq_tick(k_u64_t *frame);

/*
 * Answer the pending signals of the program the timer interrupted. The
 * signal frame is built on the stack of the program and the iretq of the
 * stub goes to the handler; a task of ring 0 has no such frame and is asked
 * again on its way out of the system call.
 */
void port_x86_64_signal_tick(k_u64_t *frame);

/*
 * The body of an exception. `cs` is the selector of the interrupted code:
 * when it names a ring 3 frame the program that faulted is taken down and
 * the machine continues, and when it names ring 0 the kernel has nowhere to
 * return to and the machine stops. The RIP and, for a page fault, CR2 say
 * where. The one exception is the first write into a page a fork made
 * read-only: the page is copied and 1 is returned, which sends the stub back
 * to the instruction that faulted with the registers it had.
 */
int port_x86_64_exception(k_u64_t vector, k_u64_t error, k_u64_t rip, k_u64_t cs);

/*
 * A fault of the program that is running: print the line of the failure and
 * end the task it runs in with the status 255. Does not return.
 */
void port_x86_64_program_fault(k_u64_t vector, k_u64_t address);

/* --- the descriptors, the page tables and the thread pointer -------------- */

/*
 * Build the GDT of the port (kernel and user selectors and a 64-bit TSS) and
 * load it. The loader has a table of its own, but only for the switch to long
 * mode: the TSS and the selectors of ring 3 are the business of the kernel.
 */
void port_x86_64_gdt_init(void);

/* The stack the processor takes on an interrupt from ring 3. */
void port_x86_64_tss_set_rsp0(k_u64_t rsp0);

/*
 * Build the identity map of the kernel. The map covers the first gigabyte in
 * 2 MiB pages, supervisor-only, the same way the loader made it; every task
 * with a program gets a copy with the user window replaced.
 */
void port_x86_64_paging_init(void);
k_u64_t port_x86_64_kernel_cr3(void);

/*
 * The page tables of one task. A space is a PML4 and the tables under it; the
 * handle is its physical address, which is also the value that goes into CR3.
 * Create builds the kernel map with the user window; clone copies the tables
 * of a parent and turns the pages of the window into copy-on-write pages of
 * both; destroy gives the tables and the frames back. Zero means failure.
 */
k_u64_t port_x86_64_paging_space_create(void);
k_u64_t port_x86_64_paging_space_clone(k_u64_t parent_cr3);
void port_x86_64_paging_space_destroy(k_u64_t cr3);

/*
 * A physical page of the arena behind the user window, already zeroed, or
 * zero when the arena is full. The frames are shared after a fork: release
 * drops one reference and the last one makes the page free again.
 */
k_u64_t port_x86_64_frame_alloc(void);
void port_x86_64_frame_release(k_u64_t paddr);

/*
 * Map `len` bytes of `vaddr` in the window of the programs onto the physical
 * pages at `paddr`, readable and writable from ring 3. Both addresses must be
 * page aligned and inside their windows.
 */
int port_x86_64_paging_map_user(k_u64_t cr3, k_u64_t vaddr, k_u64_t paddr, k_size_t len);

/* The physical address behind a virtual address of the window of programs. */
void *port_x86_64_paging_phys(k_u64_t cr3, k_u64_t vaddr);

/*
 * The copy of the page under `vaddr` for the first write after a fork. The
 * page belongs to both spaces until then; this one gets a frame of its own
 * with the write bit back. Returns 1 when the instruction may be retried.
 */
int port_x86_64_paging_handle_cow(k_u64_t cr3, k_u64_t vaddr);

/* The CR3 in the register of the processor right now. */
k_u64_t port_x86_64_current_cr3(void);

/* Make `cr3` the page table of the processor. */
void port_x86_64_paging_switch(k_u64_t cr3);

/* The thread pointer of the kernel (the TLS block of the current task) and
 * the zero a program of ring 3 sees: the kernel's block is not its memory. */
void port_x86_64_tls_kernel(void);
void port_x86_64_tls_user(void);

/*
 * Own the block devices of the port: a volume in the memory of the machine,
 * "ram0", which is where the filesystem of the port lives (blockdev_ram.c).
 * Called by port_x86_64_init(), before the kernel asks for a device.
 */
void port_x86_64_blockdev_init(void);

/*
 * Own the disk of the port: the drive on the primary ATA channel, read and
 * written by PIO (ata.c), and its partitions (ADR 0008). A machine without a
 * drive keeps only the volume in its memory. Called by port_x86_64_init().
 */
void port_x86_64_disk_init(void);

/*
 * The volume the filesystem of the boot is mounted on: the first partition of
 * the disk, or "ram0" when there is no disk.
 */
const char *port_x86_64_root_device(void);

/*
 * Own the network of the port: find the virtio-net device on the PCI bus,
 * bring it up and keep the four bytes of the configuration QEMU user-net
 * gives every guest (src/virtio_net.c for the driver, src/net.c for the
 * protocols). Called by port_x86_64_init(); a machine without a NIC keeps
 * the "no network" answers of the contract.
 */
void port_x86_64_net_init(void);

/*
 * Register the commands of the network with the shell: `netinfo`, `ping` and
 * `netudp`. The components of the network are registered by the boot when
 * they are part of the image (MICRO_OS_X86_NET, see kmain.c).
 */
int port_x86_64_net_register(void);

/*
 * The hook of the task substrate, called on every tick. Weak here: a build
 * without a scheduler keeps its clock, and the file of the tasks of the port
 * overrides it when the scheduler exists.
 */
void port_x86_64_task_tick(void);

/* --- the map of the machine ---------------------------------------------- */

/*
 * The window of the programs is the only memory of the machine that a ring 3
 * program may touch. It is mapped by the per-program page tables onto the
 * physical arena below, so the virtual address of a program and the physical
 * page behind it are two different numbers for the first time on this port.
 * The arena is outside the heap (which stops at 32 MiB) and inside the
 * identity map of the kernel, which is how the kernel writes an image the
 * program will read.
 */
#define PORT_X86_64_USER_BASE 0x02000000u  /* 32 MiB */
#define PORT_X86_64_USER_LIMIT 0x03000000u /* 48 MiB */
#define PORT_X86_64_USER_PHYS 0x04000000u  /* 64 MiB */

/* --- what the boot of the platform learned -------------------------------- */

struct port_x86_64_info {
    k_u64_t cpu_hz;       /* the TSC, calibrated against the PIT */
    k_u64_t usable_bytes; /* what the memory map of the boot calls usable */
    k_u64_t heap_total;   /* the pool behind k_port_alloc */
    k_u32_t boot_version; /* the version of the multiboot information */
    bool tsc_calibrated;  /* false when the PIT could not be read */
};

/*
 * Bring up the machine: COM1, the clock, the heap. The address of the
 * multiboot information comes from the loader; every part of the init is
 * also lazy, so a call of the contract before this one still works.
 */
void port_x86_64_init(k_u64_t multiboot_info);

/* The numbers above, valid after port_x86_64_init(). */
const struct port_x86_64_info *port_x86_64_info(void);

/* Write a line to COM1 without the kernel: for the failures of the boot. */
void port_x86_64_panic(const char *text);

#endif /* MICRO_OS_PORT_X86_64_H */
