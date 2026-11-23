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

#include "kernel/clock.h"
#include "kernel/log.h"
#include "kernel/cpu.h"
#include "kernel/port.h"
#include "kernel/port/cpu.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "port_x86_64_console.h"
#include "port_x86_64_screen.h"
#include "port_x86_64_task.h"

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
 * COM1 is a plain terminal: the SGR sequences that colour the screen mean
 * nothing in a transcript, and a file a person saves from the serial port
 * should read as the text it is. The bytes of an escape sequence are held
 * until its final byte: a sequence that ends in 'm' - the colours of the log
 * and of a command - is dropped, and every other sequence is written out as
 * it arrived, so `clear` still sends its two. A sequence that a write splits
 * in the middle is held across the two calls.
 */
#define COM1_TEXT 0u
#define COM1_ESCAPE 1u
#define COM1_CSI 2u
#define COM1_ESCAPE_MAX 16

static k_u8_t g_com1_state;
static char g_com1_escape[COM1_ESCAPE_MAX];
static k_size_t g_com1_len;

/* One byte of the dialogue on the wire, with the line ending a terminal of
 * the user wants and the kernel writes as one. */
static void com1_byte(char c)
{
    if (c == '\n') {
        while ((port_inb(COM1_LSR) & 0x20) == 0) {
        }
        port_outb(COM1, '\r');
    }

    while ((port_inb(COM1_LSR) & 0x20) == 0) {
    }
    port_outb(COM1, (k_u8_t)c);
}

static void com1_flush(void)
{
    k_size_t i;

    for (i = 0; i < g_com1_len; i++) {
        com1_byte(g_com1_escape[i]);
    }

    g_com1_len = 0;
}

static void com1_hold(char c)
{
    if (g_com1_len < sizeof(g_com1_escape)) {
        g_com1_escape[g_com1_len++] = c;
        return;
    }

    /* A sequence longer than a terminal's rendition: it is not one of ours,
     * so its bytes go out as they are and the filter starts again. */
    com1_flush();
    g_com1_state = COM1_TEXT;
    com1_byte(c);
}

static void com1_write(const char *data, k_size_t len)
{
    k_size_t i;

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (g_com1_state == COM1_TEXT) {
            if (c == 0x1B) {
                g_com1_state = COM1_ESCAPE;
                com1_hold(c);
            } else {
                com1_byte(c);
            }
            continue;
        }

        if (g_com1_state == COM1_ESCAPE) {
            com1_hold(c);
            g_com1_state = (c == '[') ? COM1_CSI : COM1_TEXT;
            if (g_com1_state == COM1_TEXT) {
                com1_flush();
            }
            continue;
        }

        /* Inside a CSI sequence: its final byte ends it. */
        com1_hold(c);
        if (c >= '@' && c <= '~') {
            bool sgr = (c == 'm');

            g_com1_state = COM1_TEXT;
            if (!sgr) {
                com1_flush();
            } else {
                g_com1_len = 0;
            }
        }
    }
}

/*
 * One call, two sinks: the window of the machine and the log of the serial
 * port. A person with the window open reads the screen and a person without
 * one reads the file, and neither of them has to know that the other exists
 * - the same bytes go to both, and the port does not have a "console" that
 * has to be chosen before the boot. The colours are the exception: the screen
 * reads them and COM1 does not get them.
 */
void k_port_write(const char *data, k_size_t len)
{
    if (data == NULL) {
        return;
    }

    /*
     * The console is one resource of the machine and a line is written byte
     * by byte to the UART: a tick in the middle of it would put the bytes of
     * another task between two of these, and a transcript would read a line
     * that was never printed. The interrupts of this core are not enough:
     * with two cores the other one writes between two bytes of this call
     * (the split line of `PROGRAM: write far 14` was exactly that), so the
     * section is the whole call and it is the big kernel lock. The lock of
     * the port is recursive per core, which is what lets the console be
     * written from a section the caller already holds and from an exception.
     */
    k_port_enter_critical();

    port_x86_64_console_write(data, len);
    com1_write(data, len);

    k_port_exit_critical();
}

/*
 * The panic of a machine with a picture: a program may hold the framebuffer
 * (ADR 0028), and the message of the last resort has to be seen. The console
 * grid comes back and the panic writes into it; COM1 carries the same bytes
 * in either case.
 */
void k_port_console_panic(void)
{
    port_x86_64_fb_console_force();
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
 * The IPI hook of the task substrate, called from the vector of
 * kernel/port_x86_64_apic.h. Weak for the same reason as the tick: a build
 * without the scheduler answers an IPI with the EOI alone.
 */
__attribute__((weak)) void port_x86_64_task_ipi(void) {}

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

        /*
         * A store of the kernel into the text of a program is not a copy of a
         * page a fork shared, but it is the same answer: this space gets a
         * private copy of the page as data, and the instruction is run again.
         * A store of ring 3 into a read-only page never comes here - its
         * error code says user (bit 2) - and is the fault of the program it
         * always was.
         */
        if ((error & 0x4u) == 0u &&
            port_x86_64_paging_allow_kernel_write(port_x86_64_current_cr3(), address)) {
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

        port_x86_64_program_fault(vector, error, address, rip);
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

void port_x86_64_interrupts_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(g_idt_pointer));
}

void port_x86_64_idt_set(k_u32_t vector, void (*handler)(void))
{
    if (vector >= IDT_VECTORS || handler == NULL) {
        return;
    }

    idt_set(vector, handler, 0, IDT_GATE_INTERRUPT);
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
    port_x86_64_interrupts_load();

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

/*
 * The heap is one list and two cores may ask for a block at the same moment:
 * the kernel task of one core and the port of another (the stack of a task is
 * allocated here, and a page of a program is not). The lock is the heap's own
 * and not the big kernel lock: a caller may already hold the BKL, and a
 * dedicated word keeps the two decisions apart. The sections are a walk of the
 * list and one split of a block - short, and with no sleep inside.
 */
static k_u32_t g_heap_lock;

/*
 * The lock takes the interrupts of the core with it. A task that holds it may
 * be preempted by the tick, and the task the scheduler switches to on the same
 * core may ask the heap for a block of its own: with the interrupts left on it
 * would spin for ever on a lock whose owner is the context it interrupted.
 * The order is the one of the big kernel lock - flags saved, interrupts off,
 * then the word - and the caller gets its flags back on the way out.
 */
static void heap_lock(k_u64_t *flags)
{
    *flags = port_save_flags_and_cli();

    while (__atomic_exchange_n(&g_heap_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&g_heap_lock, __ATOMIC_RELAXED) != 0u) {
            __asm__ volatile("pause");
        }
    }
}

static void heap_unlock(k_u64_t flags)
{
    __atomic_store_n(&g_heap_lock, 0u, __ATOMIC_RELEASE);
    port_restore_flags(flags);
}

extern char __kernel_end[];

#define BLOCK_ALIGN 16u

/* The fields of the multiboot 1 information this port reads. */
#define MB_FLAG_MODS 0x00000008u
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

/*
 * The information of the boot, kept for the questions a driver asks later.
 * It is a structure of the boot loader in the identity map, and the port
 * copies nothing out of it: it lives as long as the machine does.
 */
static const struct mb_info *g_boot_info;

/* The last byte of the modules of the boot, 0 when the boot had none. The
 * loader copied the kernel out of one of them and the volume of the ISO is
 * read from another, so the heap has to start above both. */
static k_u64_t boot_modules_end(const struct mb_info *info)
{
    const k_u32_t *mods;
    k_u64_t end = 0;
    k_u32_t i;

    if (info == NULL || (info->flags & MB_FLAG_MODS) == 0 || info->mods_addr == 0) {
        return 0;
    }

    mods = (const k_u32_t *)(uintptr_t)info->mods_addr;

    for (i = 0; i < info->mods_count; i++) {
        if ((k_u64_t)mods[i * 4u + 1u] > end) {
            end = mods[i * 4u + 1u];
        }
    }

    return end;
}

bool port_x86_64_boot_module(k_u32_t index, k_u64_t *start, k_u64_t *size)
{
    const k_u32_t *mods;

    if (g_boot_info == NULL || (g_boot_info->flags & MB_FLAG_MODS) == 0 ||
        g_boot_info->mods_addr == 0 || index >= g_boot_info->mods_count) {
        return false;
    }

    mods = (const k_u32_t *)(uintptr_t)g_boot_info->mods_addr;
    mods += (k_size_t)index * 4u;

    if (mods[1] <= mods[0]) {
        return false;
    }

    if (start != NULL) {
        *start = mods[0];
    }

    if (size != NULL) {
        *size = (k_u64_t)mods[1] - mods[0];
    }

    return true;
}

static void heap_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;
    k_u64_t best_base = 0;
    k_u64_t best_size = 0;
    /*
     * The map of the boot describes physical memory, and the heap is the low
     * identity map: a block handed out by k_port_alloc() is an address the
     * card drivers may hand to a device as it stands (src/virtio_net.c). The
     * kernel image is linked at the top, so its end has to come back to the
     * physical number before it can bound a region of the map.
     */
    k_u64_t kernel_end = port_x86_64_kernel_phys(__kernel_end);
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
     * The modules of the boot are memory as well. A loader that hands the
     * kernel a volume (the root image of the ISO, ADR 0026) leaves it in the
     * usable region of the map, and a block of the allocator over those bytes
     * would turn the filesystem of the boot into a heap that is written and
     * read at once. The heap starts above the last byte of every module; a
     * module below the region changes nothing.
     */
    {
        k_u64_t modules_end = boot_modules_end(info);

        if (modules_end > best_base && modules_end < best_base + best_size) {
            best_size -= modules_end - best_base;
            best_base = modules_end;
        }
    }

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
    void *payload = NULL;
    k_u64_t flags;

    K_UNUSED(psram); /* there is one memory on this machine */

    if (size == 0 || g_first == NULL) {
        return NULL;
    }

    size = (size + BLOCK_ALIGN - 1) & ~(k_size_t)(BLOCK_ALIGN - 1);

    heap_lock(&flags);

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
        payload = (k_u8_t *)block + sizeof(struct block);
        break;
    }

    heap_unlock(flags);

    return payload;
}

void k_port_free(void *ptr)
{
    struct block *block;
    k_u64_t flags;

    if (ptr == NULL || g_first == NULL) {
        return;
    }

    heap_lock(&flags);

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

    heap_unlock(flags);
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
 * The critical section of the machine: the big kernel lock of ADR 0012.
 *
 * The interrupts of the local core are not enough once a second core may
 * enter the kernel: `cli` stops the core that asks, not the core that writes
 * the same run queue. The kernel structures shared by every core - the run
 * queues, the task table, the heap, the VFS - are therefore serialised by one
 * global lock, and the local interrupts are turned off for the whole hold.
 *
 * The lock is recursive per core, because the kernel takes a section inside
 * another one in many places (the scheduler lock, the heap, the VFS). Only the
 * outermost entry of a core touches the machine-wide word: the nested ones
 * count, and the outermost exit releases it. The flags of every level are
 * kept, so a section that ends gives back the state it found and not an
 * unconditional "on" - an inner exit must not open the window an outer entry
 * closed.
 *
 * The acquisition order is the one Linux uses for a raw spinlock that also
 * masks an interrupt: save the flags and disable the interrupts first, then
 * spin for the word with the `pause` hint. Nothing here may sleep, switch a
 * task, copy to a user or wait for a device: every one of those paths drops
 * the lock first (kernel/port/lock.h, kernel/cpu.h). A holder that slept
 * would stop every core of the machine, and the audit of the callers is in
 * docs/debts.md (K8).
 *
 * Sixteen levels is far more than the kernel nests today; the guard is there
 * so that a bug shows up as a section that stays closed instead of a write
 * outside the array.
 */
#define CRITICAL_DEPTH_MAX 16u

/*
 * The one word of the machine. Zero is free, one is held. It is read through
 * the atomic builtins so that the compiler does not turn the test of the spin
 * into a load it may hoist out of the loop, and the release store is what
 * publishes the writes of the section to the core that takes it next.
 */
static k_u32_t g_critical_lock;

static k_u64_t g_critical_flags[KERNEL_CPUS_MAX][CRITICAL_DEPTH_MAX];
static k_u32_t g_critical_depth[KERNEL_CPUS_MAX];

static void critical_lock_acquire(void)
{
    /*
     * The polite half of a test-and-set lock: an exchange on every turn would
     * keep the cache line of the lock in the other core's cache, and the core
     * that holds it is trying to finish a short section. The first exchange is
     * the only one that must be atomic; after it fails the word is only read
     * until it reads zero, and then one exchange again settles the race
     * between two waiters.
     */
    for (;;) {
        if (__atomic_exchange_n(&g_critical_lock, 1u, __ATOMIC_ACQUIRE) == 0u) {
            return;
        }

        while (__atomic_load_n(&g_critical_lock, __ATOMIC_RELAXED) != 0u) {
            __asm__ volatile("pause");
        }
    }
}

static void critical_lock_release(void)
{
    __atomic_store_n(&g_critical_lock, 0u, __ATOMIC_RELEASE);
}

void k_port_enter_critical(void)
{
    k_u32_t cpu = k_port_cpu_id();
    k_u64_t flags;

    if (cpu >= KERNEL_CPUS_MAX) {
        cpu = 0u;
    }

    flags = port_save_flags_and_cli();

    /*
     * The outermost section of this core takes the machine. A nested one
     * finds its own depth above zero and does not touch the word: the lock is
     * already held by this core, and a second acquisition would deadlock on
     * the core that owns it.
     */
    if (g_critical_depth[cpu] == 0u) {
        critical_lock_acquire();
    }

    if (g_critical_depth[cpu] < CRITICAL_DEPTH_MAX) {
        g_critical_flags[cpu][g_critical_depth[cpu]] = flags;
    }

    g_critical_depth[cpu]++;
}

void k_port_exit_critical(void)
{
    k_u32_t cpu = k_port_cpu_id();
    k_u64_t flags;

    if (cpu >= KERNEL_CPUS_MAX) {
        cpu = 0u;
    }

    if (g_critical_depth[cpu] == 0) {
        /* An unbalanced exit says nothing about the flags to restore: the
         * safe answer is to leave the interrupts as they are, and never to
         * release a lock this core does not hold. */
        return;
    }

    g_critical_depth[cpu]--;

    flags = (g_critical_depth[cpu] < CRITICAL_DEPTH_MAX)
                ? g_critical_flags[cpu][g_critical_depth[cpu]]
                : 0u;

    if (g_critical_depth[cpu] == 0u) {
        /* Release before the flags: the next core may enter only after every
         * write of this section has been published, and the release store is
         * the barrier that publishes them. */
        critical_lock_release();
    }

    port_restore_flags(flags);
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
#define PORT_API_MINOR 10u

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

/*
 * The clock of the machine, before a driver of a real one exists. This
 * machine of QEMU has a CMOS clock, and the port does not read it yet; the
 * kernel keeps the moment of the last `k_clock_set_unix()` and the uptime of
 * that moment (kernel/clock.c), so `ntp` gives the machine a clock that
 * keeps running, and a TLS connection has a date to check a certificate
 * against (ADR 0024 §3). The setter is the port's promise that it keeps the
 * moment; the kernel holds the base.
 */
static int software_clock_set(k_u64_t unix_seconds)
{
    (void)unix_seconds;

    return 0;
}

void port_x86_64_init(k_u64_t multiboot_info)
{
    const struct mb_info *info = (const struct mb_info *)(uintptr_t)multiboot_info;

    /* The questions of the drivers about the modules of the boot are asked
     * later; the structure itself stays where the loader left it. */
    g_boot_info = info;

    /*
     * The descriptors come first: the loader's table was only the way into
     * long mode, and the TSS that gives ring 3 a stack on a call has to exist
     * before a program can make one. The page tables follow the memory they
     * describe; both are before the interrupts that use them.
     */
    port_x86_64_gdt_init();
    tls_init();
    com1_init();
    clock_measure();
    k_clock_register(software_clock_set);
    port_x86_64_cpus_init();
    heap_init(multiboot_info);
    port_x86_64_paging_init();

    /*
     * The clean state of the floating point unit, captured while nothing of
     * the kernel holds a number in a register: every context that has not
     * run starts from these bytes and the scheduler saves and loads the
     * state of a task with them (docs/debts.md, K9). Before the APs, which
     * copy the same image into their own boot context.
     */
    port_x86_64_fpu_init();

    /*
     * The screen comes after the page tables: the framebuffer of the VGA sits
     * above the identity map of the first gigabyte, so the windows of the
     * card is a mapping the port makes and not an address it already has
     * (ADR 0023). A machine without the interface keeps the text mode.
     */
    port_x86_64_console_init();

    g_info.cpu_hz = g_tsc_per_second;
    g_info.usable_bytes = 0;
    g_info.boot_version = info != NULL ? 1u : 0u;
    g_info.cpu_available = port_x86_64_cpu_available();
    g_info.cpu_started = port_x86_64_cpu_started();
    g_info.apic_base = port_x86_64_apic_base();

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
    port_x86_64_module_init();
    port_x86_64_disk_init();

    /* The network is the last device of the boot before the interrupts: it
     * asks the PCI bus for a card, and a machine without one keeps the "no"
     * answers of the contract. */
    port_x86_64_net_init();

    /* Last, because it is the point of no return: from here the timer fires
     * every millisecond and the kernel has to be able to take it. */
    port_x86_64_interrupts_init();

    /*
     * The application processors (ADR 0012). The tables of the machine and
     * the interrupts now exist, so a core can be started and reach C; the
     * scheduler does not exist yet, so the cores stop before it and kmain()
     * releases them once kernel_boot() has made the array of the cores.
     */
    port_x86_64_smp_init();

    g_info.cpu_started = port_x86_64_cpu_started();
    g_info.apic_base = port_x86_64_apic_base();
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
