/*
 * Micro OS — the port of a QEMU x86-64 machine.
 *
 * The kernel is loaded by the loader of the port in long mode with the page
 * tables already built, so this file is plain C: the console is the text
 * screen of the VGA and COM1 at once, the keys come from the PS/2 keyboard
 * or from the UART, the clock is the tick of the programmable interval
 * timer, the memory comes from the map the bootloader hands over, and a
 * critical section saves the interrupt flag and turns the interrupts off,
 * which on one core of this machine is exactly what it says.
 *
 * There is no task substrate yet (see port_x86_64_task.c): the kernel boots,
 * checks the contract, prints its banner, keeps its clock and stops.
 * Everything else grows from there.
 */
#include "port_x86_64.h"

#include <stdarg.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "port_x86_64_console.h"

/* --- the machine --- */

/*
 * The low level of the machine lives in the header, where the entry code of
 * the loader can see the same instructions.
 */

/* The block the linker made for the _Thread_local of the library. */
extern char __tls_start[];
extern char __tls_data_end[];
extern char __tls_end[];

/*
 * The library keeps its errno in a _Thread_local, and the compiler reaches it
 * through %fs. The boot context has the block of the image; every task gets a
 * copy of it (port_x86_64_task.c), because a program of ring 3 leaves the
 * thread pointer at zero and the next call must not inherit that.
 *
 * The tbss of the block is NOBITS and the loader clears only .bss, so the
 * zero-valued _Thread_local of the boot is cleared here, once, before the
 * register points at it.
 */
static void tls_init(void)
{
    memset(__tls_data_end, 0, (k_size_t)(__tls_end - __tls_data_end));
    port_wrmsr(0xC0000100u, (k_u64_t)(uintptr_t)__tls_end);
}

/* --- the console: the text screen, COM1, and the keyboard --- */

#define COM1 0x3F8
#define COM1_LSR 0x3FD /* line status: bit 5 "can write", bit 0 "can read" */

static void com1_init(void)
{
    port_outb(COM1 + 1, 0x00); /* no interrupts: the console is polled */
    port_outb(COM1 + 3, 0x80); /* DLAB on */
    port_outb(COM1 + 0, 0x01); /* divisor 1: 115200 baud */
    port_outb(COM1 + 1, 0x00);
    port_outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop */
    port_outb(COM1 + 2, 0xC7); /* FIFO on, cleared, 14 bytes */
    port_outb(COM1 + 4, 0x03); /* DTR and RTS */
}

/*
 * One call, two sinks: the window of the machine and the log of the serial
 * port. A person with the window open reads the screen and a person without
 * one reads the file, and neither of them has to know that the other exists
 * - the same bytes go to both, and the port does not have a "console" that
 * has to be chosen before the boot.
 */
void k_port_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL) {
        return;
    }

    port_x86_64_vga_write(data, len);

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            /* The terminal of the user wants both, the kernel writes one. */
            while ((port_inb(COM1_LSR) & 0x20) == 0) {
            }
            port_outb(COM1, '\r');
        }

        while ((port_inb(COM1_LSR) & 0x20) == 0) {
        }
        port_outb(COM1, (k_u8_t)c);
    }
}

int k_port_console_readline(char *buf, k_size_t size)
{
    k_size_t i = 0;

    if (buf == NULL || size == 0) {
        return -1;
    }

    for (;;) {
        int c = k_port_console_getchar();

        if (c < 0) {
            return -1;
        }

        if (c == '\r' || c == '\n') {
            break;
        }

        if (i + 1 < size) {
            buf[i++] = (char)c;
        }
    }

    buf[i] = '\0';
    return (int)i;
}

/* The interrupt flag of the processor, as the console needs to know it. */
static bool interrupts_enabled(void)
{
    k_u64_t flags;

    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
    return (flags & 0x200u) != 0;
}

/*
 * The contract says this call waits for the character, and it does: a key
 * that the keyboard put in the ring is taken at once, the byte in the buffer
 * of the UART is taken at once, and an idle machine waits for the next
 * interrupt of the timer instead of spinning - HLT is a core asleep, and the
 * tick wakes it a thousand times a second.
 *
 * The keyboard comes first: it is the device of the person in front of the
 * screen, and a serial console that is quiet has nothing to say anyway.
 */
int k_port_console_getchar(void)
{
    for (;;) {
        int key = port_x86_64_ps2_poll();

        if (key >= 0) {
            return key;
        }

        if ((port_inb(COM1_LSR) & 0x01) != 0) {
            return (int)port_inb(COM1);
        }

        if (interrupts_enabled()) {
            __asm__ volatile("hlt");
        }
    }
}

/*
 * The non-blocking reading of the same two devices. The shell of the kernel
 * polls and sleeps between two calls, and a poll that waited for a key would
 * keep the machine from doing anything else while the person thinks.
 */
int k_port_console_poll(void)
{
    int key = port_x86_64_ps2_poll();

    if (key >= 0) {
        return key;
    }

    if ((port_inb(COM1_LSR) & 0x01) != 0) {
        return (int)port_inb(COM1);
    }

    return -1;
}

/* --- the calibration of the processor clock --- */

/*
 * The time stamp counter counts at the frequency of the processor, and the
 * frequency is not written anywhere the kernel can read. It is measured
 * once, against the programmable interval timer, which has a crystal of
 * its own: channel 2 counts down 50 ms in a one-shot mode and the counter
 * is read before and after.
 *
 * The measurement is no longer how the kernel tells the time — that is the
 * ticks of the timer, see "the time" below — but it says how fast this
 * machine is, which is the first number a human asks about a port.
 */
static k_u64_t g_tsc_per_second;

static void clock_measure(void)
{
    const k_u32_t counts = 59659; /* 50 ms of the 1193182 Hz crystal */
    k_u64_t first;
    k_u64_t last;
    k_u8_t gate;

    port_outb(0x43, 0xB0); /* channel 2, mode 0 (one-shot), low then high byte */
    port_outb(0x42, (k_u8_t)(counts & 0xFF));
    port_outb(0x42, (k_u8_t)(counts >> 8));

    /* The gate of channel 2 is bit 0 of port 0x61: low clears, high starts. */
    gate = port_inb(0x61) & (k_u8_t)~0x01;
    port_outb(0x61, gate);
    port_outb(0x61, (k_u8_t)(gate | 0x01));

    first = port_rdtsc();

    /* Bit 5 of the same port is the output of the channel: high when the
     * counter reached zero. */
    while ((port_inb(0x61) & 0x20) == 0) {
    }

    last = port_rdtsc();

    g_tsc_per_second = (last - first) * 20u;
}

/* --- the interrupts --- */

/*
 * A gate of the 64-bit table: the address of the handler is kept in three
 * pieces around the selector, because the format still has its 1980s shape.
 * The code selector 0x08 is the one of the GDT the loader built, which is
 * still the table in the register of the processor.
 */
struct idt_gate {
    k_u16_t offset_low;
    k_u16_t selector;
    k_u8_t ist; /* no interrupt stack table: the stack of the moment is enough */
    k_u8_t type;
    k_u16_t offset_mid;
    k_u32_t offset_high;
    k_u32_t zero;
} __attribute__((packed));

struct idt_pointer {
    k_u16_t limit;
    k_u64_t base;
} __attribute__((packed));

#define IDT_VECTORS 256u
#define IDT_CODE_SELECTOR 0x0008u
#define IDT_GATE_INTERRUPT 0x8Eu /* present, ring 0, interrupts off on entry */
#define IDT_GATE_USER 0xEEu      /* the same, but a program of ring 3 may raise it */

/* The stack of IST1, for a double fault that finds the current stack broken. */
#define IDT_IST_DOUBLE_FAULT 1u

#define IRQ_BASE 0x20u /* the PIC is moved out of the way of the exceptions */
#define IRQ_TIMER 0u
#define IRQ_KEYBOARD 1u

#define PIT_HZ 1000u
#define PIT_CRYSTAL_HZ 1193182u

#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u

/*
 * The vector of the system calls. It is the one Linux has used on this
 * machine since 1995 and it is in the middle of nothing: the exceptions end
 * at 31, the lines of the hardware start at 32, and 128 is far from both.
 * The gate is an interrupt gate of ring 0, so the interrupts are off while
 * the stub saves its registers and on again when it says so (ADR 0006).
 */
#define IDT_SYSCALL 0x80u

static struct idt_gate g_idt[IDT_VECTORS] __attribute__((aligned(16)));
static struct idt_pointer g_idt_pointer;

/* The handlers themselves are in interrupts.S: the stub of every exception,
 * the timer, the one for a vector nobody claimed, and the table of the 32
 * exception stubs, so that this file can fill the table without naming
 * every symbol of the assembly. */
extern void (*const port_isr_exception_table[32])(void);
extern void isr_irq0(void);
extern void isr_irq1(void);
extern void isr_syscall(void);
extern void isr_unexpected(void);

static volatile k_u64_t g_ticks;

static void idt_set(k_u32_t vector, void (*handler)(void), k_u8_t ist, k_u8_t type)
{
    k_u64_t address = (k_u64_t)(uintptr_t)handler;
    struct idt_gate *gate = &g_idt[vector];

    gate->offset_low = (k_u16_t)(address & 0xFFFFu);
    gate->selector = IDT_CODE_SELECTOR;
    gate->ist = ist;
    gate->type = type;
    gate->offset_mid = (k_u16_t)((address >> 16) & 0xFFFFu);
    gate->offset_high = (k_u32_t)(address >> 32);
    gate->zero = 0;
}

/*
 * The two chips of the 8259 work in the mode the PC has used since 1981:
 * four commands in a row (ICW1..ICW4), then the mask. The exceptions take
 * the vectors 0..31, so the lines of the hardware start at 0x20.
 *
 * The timer and the keyboard are open, the rest of the machine is not
 * unmasked until the port has a driver that could answer: the mask of the
 * master is 0xFC, two zeros at the bottom for the two lines that have a
 * handler in the table.
 */
static void pic_init(void)
{
    port_outb(PIC1_COMMAND, 0x11); /* ICW1: init, an ICW4 follows */
    port_io_wait();
    port_outb(PIC2_COMMAND, 0x11);
    port_io_wait();

    port_outb(PIC1_DATA, IRQ_BASE); /* ICW2: the vector of the first line */
    port_io_wait();
    port_outb(PIC2_DATA, IRQ_BASE + 8u);
    port_io_wait();

    port_outb(PIC1_DATA, 0x04); /* ICW3: the slave hangs on IRQ2 */
    port_io_wait();
    port_outb(PIC2_DATA, 0x02); /* and this is its line there */
    port_io_wait();

    port_outb(PIC1_DATA, 0x01); /* ICW4: the 8086 mode */
    port_io_wait();
    port_outb(PIC2_DATA, 0x01);
    port_io_wait();

    port_outb(PIC1_DATA, 0xFC); /* bits 0 and 1 clear: the timer, the keyboard */
    port_outb(PIC2_DATA, 0xFF); /* nothing of the slave is */
}

/*
 * Channel 0 of the 8254 in mode 2: it counts down to zero, raises the
 * interrupt and reloads itself, so the kernel never rearms it. The crystal
 * is 1193182 Hz and the divisor is a whole number, so 1193 gives 1000.27 Hz,
 * a millisecond per tick with an error of 0.03 %, which is a clock a kernel
 * can live with.
 */
static void pit_init(k_u32_t hz)
{
    k_u32_t divisor = PIT_CRYSTAL_HZ / hz;

    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    port_outb(0x43, 0x34); /* channel 0, low byte then high, mode 2 */
    port_outb(0x40, (k_u8_t)(divisor & 0xFFu));
    port_outb(0x40, (k_u8_t)(divisor >> 8));
}

/*
 * The body of the timer, called by the stub in interrupts.S with the
 * interrupts off. The EOI goes to the master alone: the timer is the only
 * unmasked line and the slave is masked as a whole.
 */
void port_x86_64_irq_tick(k_u64_t *frame)
{
    g_ticks++;

    /* The EOI goes out before the hook: the timer must be acknowledged even
     * if the scheduler decides that this task never continues. The slave is
     * masked as a whole and the timer is its only line, so the master is the
     * only chip to tell. */
    port_outb(PIC1_COMMAND, PIC_EOI);

    /*
     * A program of ring 3 that has a signal waiting answers it when the tick
     * returns to it. The frame is the one of the interrupted code, and the
     * delivery puts the handler in it; a kernel task has no such frame and
     * is checked on its way out of the system call instead.
     */
    port_x86_64_signal_tick(frame);

    port_x86_64_task_tick();
}

/*
 * The hook of the task substrate: the tasks of the port override it to
 * decide whose turn is next. It is weak, so that a build without a
 * scheduler still links and the kernel still has its clock.
 */
__attribute__((weak)) void port_x86_64_task_tick(void) {}

/*
 * The console of the failure path: an exception prints through k_port_write
 * directly, not through the log, because the log takes a lock and may be
 * the very thing that broke.
 */
static void com1_puts(const char *text)
{
    k_size_t len = 0;

    while (text[len] != '\0') {
        len++;
    }

    k_port_write(text, len);
}

static void com1_put_dec(k_u64_t value)
{
    char digits[20];
    k_size_t count = 0;

    if (value == 0) {
        com1_puts("0");
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        count--;
        k_port_write(&digits[count], 1);
    }
}

static void com1_put_hex64(k_u64_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    /* The full width with the zeros in front: an address of the kernel is
     * long, and the eye compares the ends of two of them. */
    for (shift = 60; shift >= 0; shift -= 4) {
        char c = digits[(value >> shift) & 0xFu];

        k_port_write(&c, 1);
    }
}

static const char *exception_name(k_u64_t vector)
{
    static const char *const names[32] = {
        "divide by zero",
        "debug",
        "non-maskable interrupt",
        "breakpoint",
        "overflow",
        "bounds",
        "invalid opcode",
        "device not available",
        "double fault",
        "reserved",
        "invalid TSS",
        "segment not present",
        "stack fault",
        "general protection",
        "page fault",
        "reserved",
        "x87 floating point",
        "alignment check",
        "machine check",
        "SIMD floating point",
        "virtualisation",
        "control protection",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "hypervisor injection",
        "VMM communication",
        "security",
        "reserved",
    };

    if (vector < 32u) {
        return names[vector];
    }

    return "unknown";
}

int port_x86_64_exception(k_u64_t vector, k_u64_t error, k_u64_t rip, k_u64_t cs)
{
    /*
     * The first write into a page a fork made read-only is not a failure:
     * the page is copied for this address space and the instruction that
     * faulted is run again by the stub. The write may come from the program
     * or from the kernel on its behalf (the status of waitpid, the buffer of
     * a read), so both rings take this way.
     */
    if (vector == 14u && (error & 0x3u) == 0x3u) {
        k_u64_t address;

        __asm__ volatile("movq %%cr2, %0" : "=r"(address));

        if (port_x86_64_paging_handle_cow(port_x86_64_current_cr3(), address)) {
            return 1;
        }
    }

    /*
     * A fault of ring 3 is the failure of a program, not of the machine.
     * The address that named the failure is CR2 for a page fault and the
     * instruction itself for the other vectors; either way the program is
     * taken down and the task that waited for it gets the status, so the
     * shell and every task of the kernel stay alive.
     */
    if ((cs & 3u) != 0u) {
        k_u64_t address = rip;

        if (vector == 14u) {
            __asm__ volatile("movq %%cr2, %0" : "=r"(address));
        }

        port_x86_64_program_fault(vector, address);
    }

    com1_puts("\nx86-64: exception ");
    com1_put_dec(vector);
    com1_puts(" (");
    com1_puts(exception_name(vector));
    com1_puts("), error 0x");
    com1_put_hex64(error);
    com1_puts(", rip 0x");
    com1_put_hex64(rip);
    com1_puts(", cs 0x");
    com1_put_hex64(cs);
    com1_puts("\nx86-64: the machine stops here\n");

    k_port_halt();
    return 0;
}

void port_x86_64_unexpected(void)
{
    com1_puts("\nx86-64: an interrupt with no handler, the machine stops here\n");
    k_port_halt();
}

void port_x86_64_interrupts_init(void)
{
    k_u32_t vector;

    /* Every entry starts as "nobody expects this interrupt", so that a
     * stray vector says so instead of jumping through a zero. */
    for (vector = 0; vector < IDT_VECTORS; vector++) {
        idt_set(vector, isr_unexpected, 0, IDT_GATE_INTERRUPT);
    }

    for (vector = 0; vector < 32u; vector++) {
        /* A double fault takes IST1: the stack it interrupted may be the one
         * that broke, and a fault handler needs a stack of its own. */
        k_u8_t ist = (vector == 8u) ? IDT_IST_DOUBLE_FAULT : 0u;

        idt_set(vector, port_isr_exception_table[vector], ist, IDT_GATE_INTERRUPT);
    }

    idt_set(IRQ_BASE + IRQ_TIMER, isr_irq0, 0, IDT_GATE_INTERRUPT);

    /* The keyboard is the second line of the master, and the handler behind
     * this stub reads the byte that raised it (src/ps2.c). */
    idt_set(IRQ_BASE + IRQ_KEYBOARD, isr_irq1, 0, IDT_GATE_INTERRUPT);

    /*
     * The door of the programs of the port. Without this one line the
     * `int $0x80` of a program would land on isr_unexpected and stop the
     * machine: the vector is a part of what the port offers, not of the
     * kernel, and a port that cannot take a call says so by never setting
     * this entry. The gate opens for ring 3: a program is a user now.
     */
    idt_set(IDT_SYSCALL, isr_syscall, 0, IDT_GATE_USER);

    g_idt_pointer.limit = (k_u16_t)(sizeof(g_idt) - 1u);
    g_idt_pointer.base = (k_u64_t)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idt_pointer));

    /* The controller of the keyboard is armed before its line is opened: a
     * key that arrives into a controller nobody prepared is a key lost. */
    port_x86_64_ps2_init();
    pic_init();
    pit_init(PIT_HZ);

    /* The table is in the register of the processor and the lines are
     * programmed; from here the timer keeps the time of the kernel. */
    port_sti();
}

/* --- the time --- */

/*
 * The time of the kernel is the number of ticks of the timer. The PIT fires
 * 1000 times a second, so a tick is a millisecond and the arithmetic of the
 * scheduler cannot drift: the calibration of the TSC above is a number for
 * the information of the port, not the clock, because a counter of the
 * processor is not guaranteed to be stable on every machine QEMU imitates.
 */
k_u64_t k_port_uptime_ms(void)
{
    return g_ticks;
}

k_u64_t k_port_uptime_us(void)
{
    return g_ticks * 1000u;
}

k_u64_t port_x86_64_ticks(void)
{
    return g_ticks;
}

/* --- the memory --- */

/*
 * A first fit allocator over the largest usable region of the map of the
 * machine, with a header in front of every block: the block of the kernel
 * is small and the machine has megabytes, so this is the whole of the
 * manager the port needs. The region starts above the kernel image, which
 * the bootloader has already loaded.
 */
struct block {
    k_u64_t size; /* the size of the payload, the header is not counted */
    struct block *next;
    bool free;
};

static struct block *g_first;
static k_size_t g_heap_total;
static k_size_t g_heap_free;

extern char __kernel_end[];

#define BLOCK_ALIGN 16u

/* The fields of the multiboot 1 information this port reads. */
#define MB_FLAG_MMAP 0x00000040u
#define MB_MMAP_USABLE 1u

struct mb_info {
    k_u32_t flags;
    k_u32_t mem_lower;
    k_u32_t mem_upper;
    k_u32_t boot_device;
    k_u32_t cmdline;
    k_u32_t mods_count;
    k_u32_t mods_addr;
    k_u32_t syms[4];
    k_u32_t mmap_length;
    k_u32_t mmap_addr;
};

struct mb_mmap_entry {
    k_u32_t size;
    k_u32_t base_low;
    k_u32_t base_high;
    k_u32_t length_low;
    k_u32_t length_high;
    k_u32_t type;
};

static void heap_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;
    k_u64_t best_base = 0;
    k_u64_t best_size = 0;
    k_u64_t kernel_end = (k_u64_t)(uintptr_t)__kernel_end;
    k_u64_t cursor;
    k_u64_t limit;

    if (info == NULL || (info->flags & MB_FLAG_MMAP) == 0 || info->mmap_length == 0) {
        return;
    }

    cursor = info->mmap_addr;
    limit = cursor + info->mmap_length;

    while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
        const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;
        k_u64_t base = entry->base_low | ((k_u64_t)entry->base_high << 32);
        k_u64_t length = entry->length_low | ((k_u64_t)entry->length_high << 32);

        if (entry->type == MB_MMAP_USABLE && entry->base_high == 0 && length > 0) {
            if (base + length > kernel_end && length > best_size) {
                best_base = base;
                best_size = length;
            }
        }

        cursor += entry->size + sizeof(entry->size);
    }

    if (best_size == 0) {
        return;
    }

    /* The kernel sits inside the region: the heap starts after it, and the
     * region is cut to a sane size (the machine may have gigabytes, the
     * kernel needs megabytes). */
    if (best_base < kernel_end) {
        best_size -= kernel_end - best_base;
        best_base = kernel_end;
    }

    best_base = (best_base + BLOCK_ALIGN - 1) & ~(k_u64_t)(BLOCK_ALIGN - 1);

    /*
     * The window of the programs (k_port_exec_base, see port_exec.c) is not
     * the memory of the kernel. The heap stops below it, or a loaded image
     * would land on the blocks of the allocator and neither side would see
     * the damage until much later.
     */
    {
        k_u64_t exec_base = (k_u64_t)(uintptr_t)k_port_exec_base();

        if (exec_base > best_base && best_base + best_size > exec_base) {
            best_size = exec_base - best_base;
        }
    }

    if (best_size > 32u * 1024u * 1024u) {
        best_size = 32u * 1024u * 1024u;
    }

    if (best_size < 1024u * 1024u) {
        return;
    }

    g_first = (struct block *)(uintptr_t)best_base;
    g_first->size = best_size - sizeof(struct block);
    g_first->next = NULL;
    g_first->free = true;

    g_heap_total = (k_size_t)g_first->size;
    g_heap_free = g_heap_total;
}

void *k_port_alloc(k_size_t size, bool psram)
{
    struct block *block;

    K_UNUSED(psram); /* there is one memory on this machine */

    if (size == 0 || g_first == NULL) {
        return NULL;
    }

    size = (size + BLOCK_ALIGN - 1) & ~(k_size_t)(BLOCK_ALIGN - 1);

    for (block = g_first; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }

        /* A block that can hold the next header and a small payload is
         * split; a smaller remainder would be unusable and stays here. */
        if (block->size >= size + sizeof(struct block) + BLOCK_ALIGN) {
            struct block *rest = (struct block *)((k_u8_t *)block + sizeof(struct block) + size);

            rest->size = block->size - size - sizeof(struct block);
            rest->next = block->next;
            rest->free = true;

            block->size = size;
            block->next = rest;
        }

        block->free = false;
        g_heap_free -= block->size;

        return (k_u8_t *)block + sizeof(struct block);
    }

    return NULL;
}

void k_port_free(void *ptr)
{
    struct block *block;

    if (ptr == NULL || g_first == NULL) {
        return;
    }

    block = (struct block *)((k_u8_t *)ptr - sizeof(struct block));
    block->free = true;
    g_heap_free += block->size;

    /* The neighbours are one list, so joining is one walk from the start. */
    for (block = g_first; block != NULL && block->next != NULL; block = block->next) {
        if (block->free && block->next->free) {
            block->size += sizeof(struct block) + block->next->size;
            block->next = block->next->next;
            break;
        }
    }
}

k_size_t k_port_heap_total(void)
{
    return g_heap_total;
}

k_size_t k_port_heap_free(void)
{
    return g_heap_free;
}

k_size_t k_port_psram_total(void)
{
    return 0;
}

k_size_t k_port_psram_free(void)
{
    return 0;
}

/* --- the locks --- */

/*
 * One core, no preemption: a critical section is the interrupts off. The
 * depth is counted, because the kernel takes a section inside another one
 * in a few places, and the flags of every level are kept: a section that
 * ends must give back the state it found, not an unconditional "on", or an
 * inner one would open the window the outer one closed.
 *
 * Sixteen levels is far more than the kernel nests today; the guard is
 * there so that a bug shows up as a section that stays closed instead of a
 * write outside the array.
 */
#define CRITICAL_DEPTH_MAX 16u

static k_u64_t g_critical_flags[CRITICAL_DEPTH_MAX];
static k_u32_t g_critical_depth;

void k_port_enter_critical(void)
{
    k_u64_t flags = port_save_flags_and_cli();

    if (g_critical_depth < CRITICAL_DEPTH_MAX) {
        g_critical_flags[g_critical_depth] = flags;
    }

    g_critical_depth++;
}

void k_port_exit_critical(void)
{
    if (g_critical_depth == 0) {
        /* An unbalanced exit says nothing about the flags to restore: the
         * safe answer is to leave the interrupts as they are. */
        return;
    }

    g_critical_depth--;

    if (g_critical_depth < CRITICAL_DEPTH_MAX) {
        port_restore_flags(g_critical_flags[g_critical_depth]);
    }
}

struct k_port_mutex {
    /*
     * Who holds the lock, as a token: zero is "nobody", the address of the
     * task when a task holds it, and a value that no task can have when the
     * boot context does (kmain mounts the volume before the first task
     * exists, and MicroFS takes its lock for that too).
     */
    k_u64_t owner;
    k_u32_t depth; /* the recursive takes of the owner */
};

#define PORT_MUTEX_BOOT ((k_u64_t)1)

/* The caller as the lock sees it. The boot context has no struct task of its
 * own, and it is still exactly one context. */
static k_u64_t mutex_token(void)
{
    struct task *self = k_port_task_current();

    return self != NULL ? (k_u64_t)(uintptr_t)self : PORT_MUTEX_BOOT;
}

/*
 * The mutex of a machine with one core and a preemptive scheduler: the flag
 * of the owner and the depth are decided with the interrupts off, and a
 * caller that finds the lock taken gives the CPU away instead of spinning on
 * it - a task that holds the lock is the only thing that can release it, and
 * a caller that spins with the interrupts on would be preempted anyway.
 *
 * No task is woken by name here: the wait is a yield, and the task comes back
 * to the lock in its turn. That is coarser than a wait queue of the kernel
 * (components/kernel has no such thing yet) and it is enough for the one
 * mutex of the filesystem, whose sections are a handful of sector writes.
 */
static void mutex_wait(void)
{
    if (k_port_task_current() != NULL) {
        k_sched_yield();
        return;
    }

    /*
     * The boot context: there is nothing to switch to, and it only takes the
     * lock when no task exists to hold it. The pause is the polite form of
     * "wait a moment" on a processor that has one.
     */
    __asm__ volatile("pause");
}

k_port_mutex_t *k_port_mutex_create(void)
{
    k_port_mutex_t *mutex = (k_port_mutex_t *)k_port_alloc(sizeof(*mutex), false);

    if (mutex == NULL) {
        return NULL;
    }

    mutex->owner = 0;
    mutex->depth = 0;

    return mutex;
}

int k_port_mutex_destroy(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    k_port_enter_critical();

    if (mutex->owner != 0) {
        /* A lock that a task holds or waits for is not ours to take away:
         * the caller keeps the handle. */
        k_port_exit_critical();
        return -1;
    }

    k_port_exit_critical();

    k_port_free(mutex);

    return 0;
}

int k_port_mutex_lock(k_port_mutex_t *mutex)
{
    k_u64_t self;

    if (mutex == NULL) {
        return -1;
    }

    self = mutex_token();

    for (;;) {
        k_u64_t owner;

        k_port_enter_critical();
        owner = mutex->owner;

        if (owner == 0) {
            mutex->owner = self;
            mutex->depth = 1;
            k_port_exit_critical();
            return 0;
        }

        if (owner == self) {
            /* The task that holds it takes it again: MicroFS does that when a
             * public call reaches a helper that takes the lock too. */
            mutex->depth++;
            k_port_exit_critical();
            return 0;
        }

        k_port_exit_critical();

        mutex_wait();
    }
}

int k_port_mutex_unlock(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    k_port_enter_critical();

    if (mutex->owner != mutex_token()) {
        /* Not the holder: an unbalanced unlock says nothing about the state
         * the caller expected, and the lock is the one thing it must not
         * lose track of. */
        k_port_exit_critical();
        return -1;
    }

    if (mutex->depth > 1) {
        mutex->depth--;
        k_port_exit_critical();
        return 0;
    }

    mutex->depth = 0;
    mutex->owner = 0;

    k_port_exit_critical();

    return 0;
}

/* --- the ends of the machine --- */

void k_port_halt(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void k_port_reset(void)
{
    /* The controller of the keyboard resets the machine, and it is the one
     * port that every PC has had since 1984. */
    port_outb(0x64, 0xFE);
    k_port_halt();
}

/* --- the identity --- */

const char *k_port_name(void)
{
    return "x86_64";
}

const char *k_port_arch(void)
{
    return "x86_64";
}

#define PORT_API_MAJOR 1u
#define PORT_API_MINOR 8u

_Static_assert(PORT_API_MAJOR == KERNEL_PORT_API_VERSION_MAJOR,
               "port_x86_64 does not speak the contract of this kernel");

unsigned k_port_api_major(void)
{
    return PORT_API_MAJOR;
}

unsigned k_port_api_minor(void)
{
    return PORT_API_MINOR;
}

/* --- the store and the network: there are none on this milestone --- */

int k_port_kv_get(const char *key, char *buf, k_size_t size)
{
    K_UNUSED(key);
    K_UNUSED(buf);
    K_UNUSED(size);
    return -2;
}

int k_port_kv_set(const char *key, const char *value)
{
    K_UNUSED(key);
    K_UNUSED(value);
    return -1;
}

int k_port_kv_erase(const char *key)
{
    K_UNUSED(key);
    return -2;
}

int k_port_kv_commit(void)
{
    return 0;
}

int k_port_kv_next(k_size_t index, char *key, k_size_t size)
{
    K_UNUSED(index);
    K_UNUSED(key);
    K_UNUSED(size);
    return -2;
}

/* The network of the port is src/net.c and src/virtio_net.c: the contract is
 * there, and a machine without a card answers "no" from it. */

/* --- the strings the compiler may ask for (there is no libc here) --- */

void *memcpy(void *dst, const void *src, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    const k_u8_t *from = (const k_u8_t *)src;
    k_size_t i;

    for (i = 0; i < size; i++) {
        to[i] = from[i];
    }

    return dst;
}

void *memset(void *dst, int value, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    k_size_t i;

    for (i = 0; i < size; i++) {
        to[i] = (k_u8_t)value;
    }

    return dst;
}

void *memmove(void *dst, const void *src, k_size_t size)
{
    k_u8_t *to = (k_u8_t *)dst;
    const k_u8_t *from = (const k_u8_t *)src;
    k_size_t i;

    if (to < from) {
        for (i = 0; i < size; i++) {
            to[i] = from[i];
        }
    } else {
        for (i = size; i > 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }

    return dst;
}

int memcmp(const void *left, const void *right, k_size_t size)
{
    const k_u8_t *a = (const k_u8_t *)left;
    const k_u8_t *b = (const k_u8_t *)right;
    k_size_t i;

    for (i = 0; i < size; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}

/* --- the init of the port --- */

static struct port_x86_64_info g_info;

void port_x86_64_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;

    /*
     * The descriptors come first: the loader's table was only the way into
     * long mode, and the TSS that gives ring 3 a stack on a call has to exist
     * before a program can make one. The page tables follow the memory they
     * describe; both are before the interrupts that use them.
     */
    port_x86_64_gdt_init();
    tls_init();
    com1_init();
    port_x86_64_vga_init();
    clock_measure();
    heap_init(multiboot_info);
    port_x86_64_paging_init();

    g_info.cpu_hz = g_tsc_per_second;
    g_info.usable_bytes = 0;
    g_info.boot_version = info != NULL ? 1u : 0u;

    if (info != NULL && (info->flags & MB_FLAG_MMAP) != 0) {
        k_u64_t cursor = info->mmap_addr;
        k_u64_t limit = cursor + info->mmap_length;

        while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
            const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;

            if (entry->type == MB_MMAP_USABLE) {
                g_info.usable_bytes +=
                    (k_u64_t)entry->length_low | ((k_u64_t)entry->length_high << 32);
            }

            cursor += entry->size + sizeof(entry->size);
        }
    }

    g_info.heap_total = g_heap_total;
    g_info.tsc_calibrated = g_tsc_per_second != 0;

    /* The volume of the port is registered before the kernel looks for one:
     * the filesystem mounts a device by name and the name has to be there. */
    port_x86_64_blockdev_init();
    port_x86_64_disk_init();

    /* The network is the last device of the boot before the interrupts: it
     * asks the PCI bus for a card, and a machine without one keeps the "no"
     * answers of the contract. */
    port_x86_64_net_init();

    /* Last, because it is the point of no return: from here the timer fires
     * every millisecond and the kernel has to be able to take it. */
    port_x86_64_interrupts_init();
}

const struct port_x86_64_info *port_x86_64_info(void)
{
    return &g_info;
}

void port_x86_64_panic(const char *text)
{
    k_size_t len = 0;

    if (text == NULL) {
        return;
    }

    while (text[len] != '\0') {
        len++;
    }

    k_port_write(text, len);
    k_port_halt();
}
