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

/*
 * A run of words through the same port, the way the BIOS moved a sector of a
 * disk: one instruction, the count in RCX and the buffer in RDI or RSI. The
 * two loops of `inw`/`outw` of a driver do the same job one word at a time,
 * and on a machine that emulates an I/O port in software every one of those
 * words is a trap of the emulator - 256 of them per sector of a disk. The
 * repeated instruction enters the emulator once for the whole run.
 *
 * The buffer is the bytes of the caller and not a run of words: a word of
 * the port is little endian and so is the memory, so what the drive sends
 * lands in the buffer in the order the filesystem expects, with no swap. The
 * two instructions do not ask for an aligned buffer either.
 */
static inline void port_insw(k_u16_t port, void *buf, k_size_t words)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline void port_outsw(k_u16_t port, const void *buf, k_size_t words)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

/*
 * The same run, four bytes at a time. The data register of a disk is 16 bits
 * wide, and a 32-bit access to it is two of those words in one turn of the
 * bus - the 32-bit PIO of every PC made after 1994. The order is the one of
 * the memory: the low word first.
 */
static inline void port_insl(k_u16_t port, void *buf, k_size_t words)
{
    __asm__ volatile("rep insl" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}

static inline void port_outsl(k_u16_t port, const void *buf, k_size_t words)
{
    __asm__ volatile("rep outsl" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
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

/* The other half of it: the register of the machine that carries the state of
 * the crossing of a system call (src/syscall.c). */
static inline k_u64_t port_rdmsr(k_u32_t msr)
{
    k_u32_t low;
    k_u32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

    return ((k_u64_t)high << 32) | low;
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
 * The two lines of the port that have a handler, in the order the counters
 * below and the `smp irq` command of the shell name them. Every other line of
 * the machine is masked: the port has no driver that could answer one.
 */
#define PORT_X86_64_IRQ_TIMER 0u
#define PORT_X86_64_IRQ_KEYBOARD 1u

/*
 * The end of an interrupt of one of those lines: the EOI of the local APIC of
 * the core when the I/O APIC delivered it (ADR 0012), the EOI of the 8259
 * when the legacy chip did. The body of a vector calls this and does not have
 * to know which chip raised the line.
 */
void port_x86_64_irq_eoi(void);

/* True when the I/O APIC delivers the two lines and the 8259 is masked. */
bool port_x86_64_irq_on_ioapic(void);

/* Count an interrupt of a line on the core that took it, and read the count of
 * one core. A line the I/O APIC routes to a core arrives on that core, and the
 * pair of numbers is how a run proves where it arrived. */
void port_x86_64_irq_note(k_u32_t line);
k_u32_t port_x86_64_irq_count(k_u32_t line, k_u32_t cpu);

/*
 * The C side of the stubs of interrupts.S. An exception says which vector
 * fired, the error code the processor pushed (0 when the vector has none)
 * and the RIP of the moment; both functions print and stop the machine,
 * because neither has anywhere to return to in a kernel this small.
 */
void port_x86_64_unexpected(void);

/*
 * The fast door of a program: the stub the IA32_LSTAR register names, which
 * the instruction `syscall` jumps to (src/interrupts.S). The vector 0x80 of
 * the IDT stays, because a program that is not the library of the tree and
 * the return of a signal handler still use it, and because a machine of two
 * doors costs nothing.
 */
extern void isr_syscall_fast(void);

/*
 * The selectors of the fast door. SYSCALL takes the code selector from
 * STAR[31:16], and the data selector is the descriptor eight bytes above it,
 * so the pair is the kernel's own 0x08 and 0x10.
 *
 * SYSRET is told one number and derives both of its selectors from it: CS is
 * STAR[63:48] + 16 and SS is STAR[63:48] + 8. The ring 3 selectors of the
 * iretq frames (0x18 code, 0x20 data) are in the wrong order for that, and
 * swapping them would rewrite every frame of a signal, a fork and an exec; so
 * the GDT carries a second pair, laid out the way the instruction wants it,
 * and STAR[63:48] is the number below (src/gdt.c).
 */
#define PORT_X86_64_SYSCALL_CS 0x08u
#define PORT_X86_64_SYSRET_STAR 0x30u

/*
 * Open the fast door on this core: IA32_EFER.SCE, IA32_STAR (the selectors),
 * IA32_LSTAR (the entry) and IA32_FMASK (the flags the processor clears on the
 * way in). Every one of them is a register of the core, so the boot calls this
 * once for itself and an application processor calls it on its way up. It is
 * called at the end of port_x86_64_interrupts_init(), after the table the
 * `int $0x80` door needs, so that a port without one of the two doors is a
 * port that never set one of the two entries.
 */
void port_x86_64_syscall_init_cpu(void);

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
 * end the task it runs in with the status 255. Does not return. `error` is
 * the error code the processor pushed: for a page fault bit 4 says that the
 * instruction fetch was the access, which is how a fetch from an NX page is
 * told from a read or a write. `address` is CR2 for a page fault and the RIP
 * for the other vectors; `rip` is always the instruction that was running.
 */
void port_x86_64_program_fault(k_u64_t vector, k_u64_t error, k_u64_t address, k_u64_t rip);

/* --- the descriptors, the page tables and the thread pointer -------------- */

/*
 * Build the GDT of the port (kernel and user selectors and a 64-bit TSS) and
 * load it. The loader has a table of its own, but only for the switch to long
 * mode: the TSS and the selectors of ring 3 are the business of the kernel.
 */
void port_x86_64_gdt_init(void);

/*
 * Build and load the table of one core: the BSP calls this for 0 at the boot,
 * an AP for its own number on the way up. The tables are identical except for
 * the TSS, which carries the RSP0 and the IST stack of that core.
 */
void port_x86_64_gdt_init_cpu(k_u32_t cpu);

/* The stack the processor takes on an interrupt from ring 3. */
void port_x86_64_tss_set_rsp0(k_u64_t rsp0);
k_u64_t port_x86_64_tss_rsp0(void);

/*
 * Build the map of the kernel: the first gigabyte identity mapped, 2 MiB
 * pages writable and NX, except the chunks of the kernel image itself, which
 * are 4 KiB pages - the text readable and executable, the rodata read-only
 * and NX, and data, bss and the stacks writable and NX. Every task with a
 * program gets a copy with the user window replaced (ADR 0007, NX and W^X).
 */
void port_x86_64_paging_init(void);

/*
 * The protection the map keeps in a register of the core: CR0.WP, which makes
 * the kernel honour the write bit of its own tables. The boot calls it for
 * itself inside port_x86_64_paging_init(); an application processor calls it
 * on its way up, because the register is per core.
 */
void port_x86_64_paging_init_cpu(void);

k_u64_t port_x86_64_kernel_cr3(void);

/*
 * Map a window of a device into the top 2 GiB, at `vaddr` (2 MiB aligned),
 * over the physical range that starts in the page of `paddr`. The pages are
 * writable and NX, and every address space of the machine shares them. The
 * answer is the number of 2 MiB pages mapped, zero when the range is not one
 * the table of the higher half covers. The screen of the port is the user of
 * the call: it maps the framebuffer of the VGA (ADR 0023).
 */
k_u32_t port_x86_64_paging_map_device(k_u64_t vaddr, k_u64_t paddr, k_size_t len);

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
 * pages at `paddr`. Both addresses must be page aligned and inside their
 * windows. The protection is the pair of flags below: a page of code is
 * executable and read-only, every page of data is writable and NX, and the
 * two are never one page (ADR 0007, NX and W^X).
 */
#define PORT_X86_64_PAGE_WRITE 0x1u /* ring 3 may write the page */
#define PORT_X86_64_PAGE_EXEC 0x2u  /* the processor may fetch from it */

int port_x86_64_paging_map_user(k_u64_t cr3, k_u64_t vaddr, k_u64_t paddr, k_size_t len,
                                k_u32_t flags);

/* The physical address behind a virtual address of the window of programs. */
void *port_x86_64_paging_phys(k_u64_t cr3, k_u64_t vaddr);

/*
 * The store of the kernel into a read-only page of a program: this space gets
 * a private copy of the page as data (writable and NX) so that the store can
 * happen, and the code of any space that shares the old frame is untouched.
 * Returns 1 when the instruction may be retried; see
 * port_x86_64_paging_allow_kernel_write() in src/paging.c.
 */
int port_x86_64_paging_allow_kernel_write(k_u64_t cr3, k_u64_t vaddr);

/*
 * The number of pages of a space that are both writable and executable. Zero
 * for a map this port built; a non-zero answer is a bug in the tables and the
 * callers say so instead of running the code.
 */
k_u32_t port_x86_64_paging_audit(k_u64_t cr3);

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
 * Open the line of the disk (IRQ 14) once the table of the interrupts and the
 * 8259 exist: port_x86_64_disk_init() probes the bus master and keeps its
 * registers, and this call is what turns the bus master from "the machine has
 * one" into "the driver uses one" (ADR 0032). Called by port_x86_64_init()
 * after port_x86_64_interrupts_init().
 */
void port_x86_64_disk_irq_init(void);

/*
 * Own the medium of the boot: a boot loader may hand the kernel more than the
 * image, and the second module of the x86-64 boot is a whole MicroFS disk (the
 * root image of the ISO, ADR 0026). It is the bytes of the boot medium in the
 * memory of the machine and nothing writes them, so the device is read-only
 * ("boot0") and its first partition is the root when the boot came from that
 * medium. Called by port_x86_64_init() after the RAM volume.
 */
void port_x86_64_module_init(void);

/* The first partition of the boot module, NULL when the boot had none. */
const char *port_x86_64_module_root(void);

/*
 * One module of the multiboot information: 0 is the kernel, 1 the root image.
 * False when the boot had no such module and the two numbers are untouched.
 * The range is physical and lives in the identity map of the first gigabyte.
 */
bool port_x86_64_boot_module(k_u32_t index, k_u64_t *start, k_u64_t *size);

/*
 * The volume the filesystem of the boot is mounted on: the root image of the
 * boot medium when there is one, the first partition of the disk, or "ram0"
 * when the machine has neither.
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

/*
 * Read the processors of the machine from the tables of the firmware (ACPI:
 * the root pointer, the RSDT/XSDT and the MADT) and remember the local APIC.
 * The port brings up the processor of the boot only; the boot prints how many
 * the firmware reports and how many are in use, and ADR 0012 has the rest of
 * the plan (the trampoline, INIT/SIPI, the APIC timer and the I/O APIC).
 * Called by port_x86_64_init() before the scheduler exists.
 */
void port_x86_64_cpus_init(void);

/* The processors the firmware calls enabled, and the ones that run: 1 today. */
k_u32_t port_x86_64_cpu_available(void);
k_u32_t port_x86_64_cpu_started(void);

/* The physical address of the local APIC from the MADT, 0 when there is none. */
k_u32_t port_x86_64_apic_base(void);

/*
 * The I/O APIC the MADT describes (ADR 0012): its physical address, 0 when
 * the firmware named none, and the line of the ISA bus an interrupt reaches
 * the chip on - the override of the table, or the number of the line itself
 * when the firmware renamed nothing. A PC renames IRQ0 to the GSI 2, because
 * that is the input the cascade of the 8259 would use. `flags` is the
 * polarity and the trigger of the override (ACPI 6.x, 5.2.12.5).
 */
k_u32_t port_x86_64_ioapic_base(void);
k_u32_t port_x86_64_ioapic_gsi(k_u32_t isa_irq);
k_u16_t port_x86_64_ioapic_irq_flags(k_u32_t isa_irq);

/*
 * Load the table of the interrupts the boot built into this core. The table
 * is one for the machine and does not change after the boot; an application
 * processor calls this on its way up, because the register that names it is
 * per core. Does not touch the 8259 or the PIT: that programming belongs to
 * the processor of the boot.
 */
void port_x86_64_interrupts_load(void);

/*
 * Put a handler of the port in the table of the interrupts, as a ring 0 gate
 * that turns the interrupts off on entry (the two constants above are an
 * implementation detail of the table). A driver of the port - the APIC of
 * ADR 0012 is the first - registers its vectors here instead of reaching into
 * the table of port_x86_64.c. The table is loaded already and lives in the
 * memory of the kernel, so a registration after the lidt is seen by every
 * core at the next interrupt.
 */
void port_x86_64_idt_set(k_u32_t vector, void (*handler)(void));

/*
 * Open one line of the 8259: the driver of a device that raises an interrupt
 * calls this after it has put its handler in the table, and the line of the
 * slave takes the cascade of the master with it. The line of a device that is
 * not there is not opened by anybody, and a line that stays masked is a
 * device the driver polls - the disk of 1.5.0 was exactly that.
 */
void port_x86_64_irq_unmask(k_u32_t line);

/* --- the processors of the machine (ADR 0012) ----------------------------- */

/*
 * Bring up the application processors the firmware reports: map the APIC,
 * give every core a stack and a table, write the parameters of the
 * trampoline, send INIT and the start-up IPI, and wait for each core to
 * answer from C. Called at the end of port_x86_64_init(), after the page
 * tables and the table of the interrupts exist. A kernel built without
 * KERNEL_SMP does nothing here and reports one core.
 *
 * The answering core stops before the scheduler and waits for
 * port_x86_64_smp_release(): the scheduler of the kernel does not exist yet
 * when the port is brought up, and a core that entered it would find an
 * array of one. kmain() releases them once kernel_boot() has run.
 */
void port_x86_64_smp_init(void);
void port_x86_64_smp_release(void);

/* The cores that answered the start-up IPI and reached C. This is the number
 * k_port_cpu_count() promises the scheduler, and the number the boot prints
 * as "in use": a core the firmware lists but this port did not start is not
 * counted. */
k_u32_t port_x86_64_smp_checked_in(void);

/* True when the APIC of the port is mapped and enabled (the bring-up needs
 * it; the number above says how many cores it actually produced). */
bool port_x86_64_smp_apic_ready(void);

/*
 * The other core asked this one to look at its run queue. Called from the
 * body of the IPI vector with the interrupts off; it takes the scheduler
 * lock, claims the next task of this core and switches to it when the core
 * has one to run or is idle. A tick of the local APIC timer may do the same
 * at the next millisecond, so this is the short path and not the only one.
 */
void port_x86_64_task_ipi(void);

/*
 * What the core did with the tasks it was given (ADR 0012, docs/debts.md K8):
 * how many tasks it started in the body of the wake interrupt - that is,
 * before its own timer could have offered them - and how many it started at
 * its tick, with the tick of the core at the last wake. The `smp` command of
 * the shell prints the three numbers, and the smoke test of the two cores
 * reads them as the proof that a wake does not wait for a millisecond.
 */
k_u32_t port_x86_64_wake_before_tick(k_u32_t cpu);
k_u32_t port_x86_64_wake_tick(k_u32_t cpu);
k_u32_t port_x86_64_tick_start(k_u32_t cpu);

/*
 * The clock of a core, stopped and started by the command of the shell that
 * proves the wake: a request rides on the vector of the interrupt of another
 * core and the core applies it in the body. With the clock stopped, a task
 * can only begin there through the interrupt - which is exactly what the
 * experiment has to show - and port_x86_64_timer_off() is the flag the core
 * wrote when it did it. `smp wake` starts the clock again before it returns.
 */
#define PORT_X86_64_REQUEST_TIMER_OFF 0x1u
#define PORT_X86_64_REQUEST_TIMER_ON 0x2u

void port_x86_64_timer_off_request(k_u32_t cpu);
void port_x86_64_timer_on_request(k_u32_t cpu);
bool port_x86_64_timer_off(k_u32_t cpu);

/* --- the map of the machine ---------------------------------------------- */
/*
 * Where the kernel is linked and where the loader puts it (ADR 0007). The
 * image lives in the top 2 GiB of the address space, the region the small
 * model of the C compiler can name with one sign-extended 32-bit immediate;
 * the loader copies the bytes to KERNEL_PHYS and maps them at KERNEL_BASE,
 * so the low identity map of the first gigabyte stays what it is for the
 * devices, the heap and the arena of the programs.
 *
 * The two numbers appear again in linker64.ld and loader32.S, which cannot
 * include a C header: the linker script names them as symbols, the loader as
 * .set constants, and this header is the third copy for the C code.
 */
#define PORT_X86_64_KERNEL_BASE 0xFFFFFFFF80000000ull
#define PORT_X86_64_KERNEL_PHYS 0x01000000ull

/*
 * The physical address behind a pointer into the image of the kernel: the
 * page tables, the GDT, the TSS and the stacks are linked high, and a page
 * table entry has to name the frame, not the link address. Only for the
 * image: a pointer of the heap is a low address that is already physical.
 */
static inline k_u64_t port_x86_64_kernel_phys(const void *ptr)
{
    return (k_u64_t)(uintptr_t)ptr - PORT_X86_64_KERNEL_BASE + PORT_X86_64_KERNEL_PHYS;
}

/*
 * The address a device sees for a buffer of the kernel. The heap is the low
 * identity map - a block of k_port_alloc() is its own bus address - while the
 * image is the high half; this is the one function a driver needs to tell the
 * two apart (the rings and the buffers of src/virtio_net.c).
 */
static inline k_u64_t port_x86_64_bus_phys(const void *ptr)
{
    k_u64_t address = (k_u64_t)(uintptr_t)ptr;

    return address >= PORT_X86_64_KERNEL_BASE ? port_x86_64_kernel_phys(ptr) : address;
}

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
    k_u64_t cpu_hz;        /* the TSC, calibrated against the PIT */
    k_u64_t usable_bytes;  /* what the memory map of the boot calls usable */
    k_u64_t heap_total;    /* the pool behind k_port_alloc */
    k_u32_t boot_version;  /* the version of the multiboot information */
    k_u32_t cpu_available; /* the processors the firmware (MADT) calls enabled */
    k_u32_t cpu_started;   /* the processors the port has brought up */
    k_u32_t apic_base;     /* the local APIC of the MADT, 0 when there is none */
    bool tsc_calibrated;   /* false when the PIT could not be read */
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
