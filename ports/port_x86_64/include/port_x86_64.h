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
void port_x86_64_exception(k_u64_t vector, k_u64_t error, k_u64_t rip);
void port_x86_64_unexpected(void);

/* The body of the timer interrupt, called by interrupts.S. */
void port_x86_64_irq_tick(void);

/*
 * The hook of the task substrate, called on every tick. Weak here: a build
 * without a scheduler keeps its clock, and the file of the tasks of the port
 * overrides it when the scheduler exists.
 */
void port_x86_64_task_tick(void);

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
