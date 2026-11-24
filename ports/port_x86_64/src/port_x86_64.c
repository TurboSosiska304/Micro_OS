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
#include "kernel/console.h"
#include "kernel/log.h"
#include "kernel/mm.h"
#include "kernel/cpu.h"
#include "kernel/port.h"
#include "kernel/port/cpu.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "port_x86_64_console.h"
#include "port_x86_64_screen.h"
#include "port_x86_64_task.h"
#include "port_x86_64_apic.h"
#include "port_x86_64_ahci.h"
#include "port_x86_64_xhci.h"

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

        /* The keyboard of the USB comes after the one of the PS/2 and
         * before the serial port: a machine has one of the two keyboards
         * and the port does not care which (ADR 0040). */
        key = port_x86_64_usb_poll();

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

    key = port_x86_64_usb_poll();

    if (key >= 0) {
        return key;
    }

    if ((port_inb(COM1_LSR) & 0x01) != 0) {
        /* The serial port has no interrupt of its own in this port: the byte
         * is found by a poll, and finding it is the moment a task that waits
         * for the console in `poll` can be told. */
        k_console_input_notify();

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

    /*
     * The gate of channel 2 is bit 0 of port 0x61, and bit 1 of the same port
     * is the speaker's own gate: a machine whose firmware left it high turns
     * this calibration into fifty milliseconds of tone. The speaker stays off
     * on both sides of the measurement - a machine that beeps while it only
     * measures its clock is a machine that looks broken.
     */
    gate = port_inb(0x61) & (k_u8_t) ~(0x01 | 0x02);
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
#define IRQ_CASCADE 2u
#define IRQ_MOUSE 12u          /* the second port of the 8042, on the slave (1.7.0) */
#define IRQ_ATA 14u            /* the primary IDE channel, on the slave */
#define IRQ_CASCADE_LINE 2u    /* the slave reaches the master through it */
#define IRQ_SPURIOUS_MASTER 7u /* the vector of an acknowledge of nothing */
#define IRQ_SPURIOUS_SLAVE 15u /* the same, on the slave */

/* The lines of the port that have a handler, in the order the counters below
 * and the `smp irq` command name them (PORT_X86_64_IRQ_TIMER and
 * PORT_X86_64_IRQ_KEYBOARD of port_x86_64.h). */
#define PORT_IRQ_LINES 2u

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
extern void isr_irq12(void);
extern void isr_irq_ata(void);
extern void isr_irq_8259_spurious(void);
extern void isr_syscall(void);
extern void isr_unexpected(void);

static volatile k_u64_t g_ticks;

/*
 * The interrupts of the two lines of the port, one counter per core, because
 * the question the two-core machine asks is where they arrive: the I/O APIC
 * of ADR 0012 delivers a line to the core its entry names, and the 8259
 * delivers every line to the core of the boot. The counters are the answer of
 * the `smp irq` command and of the smoke test of the two cores.
 */
static k_u32_t g_irq_count[PORT_IRQ_LINES][KERNEL_CPUS_MAX];

/* True when the I/O APIC delivers the two lines of the port and the 8259 is
 * masked (see interrupts_init). */
static bool g_ioapic_lines;

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
 * handler in the table. This is the fallback of the port: a machine whose
 * firmware describes an I/O APIC gets its lines through that chip instead and
 * the 8259 is masked as a whole (pic_mask_all).
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

    /*
     * The timer, the keyboard and the pointer are open; nothing else is. The
     * mouse hangs on the fourth line of the slave, so the cascade of the
     * second chip is open as well (1.7.0): the two bits of the slave that are
     * clear are the cascade and the pointer.
     */
    port_outb(PIC1_DATA, 0xF8); /* bits 0, 1 and 2 clear: timer, keyboard, cascade */
    port_outb(PIC2_DATA, 0xEF); /* bit 4 clear: the mouse of IRQ12 */
}

/* Close both chips: every line of the 8259 is masked and the chip raises
 * nothing. The port does this when the I/O APIC takes the two lines of the
 * machine, so that a line cannot arrive twice. */
static void pic_mask_all(void)
{
    port_outb(PIC1_DATA, 0xFF);
    port_outb(PIC2_DATA, 0xFF);
}

/* Open the two lines of the port on the 8259 again (the fallback of a machine
 * that has no I/O APIC, and the undo of a route that did not hold). */
static void pic_unmask_port_lines(void)
{
    port_outb(PIC1_DATA, 0xF8);
    port_outb(PIC2_DATA, 0xEF);
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
 * The end of an interrupt of the two lines of the port.
 *
 * A line the I/O APIC delivers is a message to the local APIC of the core it
 * named, and the end of it is a store to the EOI register of that core's own
 * APIC (every core decodes the same physical page as itself). A line of the
 * 8259 is ended in the chip that raised it. The port says which of the two is
 * the source of the lines, and the body of the vector does not have to know.
 */
void port_x86_64_irq_eoi(void)
{
    if (g_ioapic_lines) {
        port_x86_64_apic_eoi();
        return;
    }

    port_outb(PIC1_COMMAND, PIC_EOI);
}

/* True when the two lines of the port are delivered by the I/O APIC and the
 * 8259 is masked. The `smp irq` command and the body of the keyboard ask. */
bool port_x86_64_irq_on_ioapic(void)
{
    return g_ioapic_lines;
}

/* Count an interrupt of a line on the core that took it. The `smp irq`
 * command reads the numbers; a core that is not in the array of the kernel is
 * not counted. */
void port_x86_64_irq_note(k_u32_t line)
{
    k_u32_t cpu = k_port_cpu_id();

    if (line >= PORT_IRQ_LINES || cpu >= KERNEL_CPUS_MAX) {
        return;
    }

    (void)__atomic_add_fetch(&g_irq_count[line][cpu], 1u, __ATOMIC_RELAXED);
}

k_u32_t port_x86_64_irq_count(k_u32_t line, k_u32_t cpu)
{
    if (line >= PORT_IRQ_LINES || cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return __atomic_load_n(&g_irq_count[line][cpu], __ATOMIC_RELAXED);
}

/*
 * The body of the timer, called by the stub in interrupts.S with the
 * interrupts off. The EOI goes out before the hook: the timer must be
 * acknowledged even if the scheduler decides that this task never continues,
 * and with the I/O APIC the line is in service until the local APIC of this
 * core hears about it.
 */
/*
 * The acknowledge that found nothing behind the request, on the 8259. The
 * cascade of the slave is the one the chip puts in service itself, so both
 * chips are told that the interrupt is over; a chip with nothing in service
 * makes a no-op of the write. A handler for these three vectors is what keeps
 * a level-triggered line of the PCI bus - the disk of ADR 0037 is one - from
 * stopping the machine on a race of the chip that no device caused.
 */
void port_x86_64_irq_8259_spurious(k_u64_t *frame)
{
    K_UNUSED(frame);

    port_outb(PIC2_COMMAND, PIC_EOI);
    port_outb(PIC1_COMMAND, PIC_EOI);
}

void port_x86_64_irq_tick(k_u64_t *frame)
{
    g_ticks++;
    port_x86_64_irq_note(PORT_X86_64_IRQ_TIMER);

    port_x86_64_irq_eoi();

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
     * A page fault of the window of the programs is one of three things: the
     * first write into a page a fork made read-only (copy on write), a store
     * of the kernel into the text of a program, or a page of a mapping of the
     * program that is not there yet (kernel/mm.h). All three are answered by
     * making the page right and running the instruction again; anything else
     * is the fault of the program, and a fault of ring 3 is the end of the
     * task and not of the machine.
     */
    if (vector == 14u) {
        k_u64_t address;
        bool write = (error & 0x2u) != 0;
        bool present = (error & 0x1u) != 0;

        __asm__ volatile("movq %%cr2, %0" : "=r"(address));

        if (present) {
            if (port_x86_64_paging_handle_cow(port_x86_64_current_cr3(), address)) {
                return 1;
            }

            /*
             * A store of the kernel into the text of a program is not a copy
             * of a page a fork shared, but it is the same answer: this space
             * gets a private copy of the page as data, and the instruction is
             * run again. A store of ring 3 into a read-only page never comes
             * here - its error code says user (bit 2) - and is the fault of
             * the program it always was.
             */
            if ((error & 0x4u) == 0u &&
                port_x86_64_paging_allow_kernel_write(port_x86_64_current_cr3(), address)) {
                return 1;
            }
        }

        /*
         * The page of a mapping: it is made, filled from the file or with
         * zeros, and the instruction that faulted runs again. The fill reads
         * the filesystem, which takes a lock and waits for the disk, so the
         * interrupts of the machine are on for it - the stub of an exception
         * enters with them off, and the task that holds the lock of the
         * volume has to be able to run. The iretq of the stub puts the flags
         * of the interrupted code back.
         */
        __asm__ volatile("sti" : : : "memory");

        if (k_mm_fault(k_mm_current(), address, write) == 0) {
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

/*
 * Open a line of the 8259. The mask register is read back, one bit is
 * cleared, and the write is done with the interrupts off: a line that is
 * opened in the middle of the read and the write would see the other lines
 * closed for an instant, which is a keyboard byte lost and not a crash, and
 * still worth the two instructions.
 *
 * A line of the slave opens the second line of the master as well: without
 * the cascade the slave answers nothing, whatever its own mask says.
 */
void port_x86_64_irq_unmask(k_u32_t line)
{
    k_u64_t flags;

    if (line >= 16u) {
        return;
    }

    flags = port_save_flags_and_cli();

    if (line < 8u) {
        port_outb(PIC1_DATA, (k_u8_t)(port_inb(PIC1_DATA) & ~(k_u8_t)(1u << line)));
    } else {
        port_outb(PIC2_DATA, (k_u8_t)(port_inb(PIC2_DATA) & ~(k_u8_t)(1u << (line - 8u))));
        port_outb(PIC1_DATA, (k_u8_t)(port_inb(PIC1_DATA) & ~(k_u8_t)(1u << IRQ_CASCADE)));
    }

    port_restore_flags(flags);
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

    /* The pointer is the twelfth line: the second device of the same
     * controller (1.7.0), and the same body with the status byte deciding. */
    idt_set(IRQ_BASE + IRQ_MOUSE, isr_irq12, 0, IDT_GATE_INTERRUPT);

    /*
     * The disk of the machine, the sixth line of the slave. The entry is here
     * for the machine that has the drive: a channel with nothing on it never
     * raises the line, and the line stays masked until the driver of the disk
     * opens it (src/ata.c, ADR 0032).
     */
    idt_set(IRQ_BASE + IRQ_ATA, isr_irq_ata, 0, IDT_GATE_INTERRUPT);

    /*
     * The three vectors of the 8259 that no device owns: the cascade, which is
     * the line of the slave on the master, and the two the chip answers with
     * when an acknowledge finds nothing behind the request. A line of the PCI
     * bus is level-triggered (the ELCR of the PIIX says which of them are), and
     * a level that lets go of its request between the interrupt and the
     * acknowledge is exactly that: the chip answers with the vector of its own
     * line, and a machine without an entry for it stops on a fault that no
     * device caused. The handler acknowledges and returns (src/interrupts.S,
     * the story is in src/virtio_blk.c).
     */
    idt_set(IRQ_BASE + IRQ_CASCADE_LINE, isr_irq_8259_spurious, 0, IDT_GATE_INTERRUPT);
    idt_set(IRQ_BASE + IRQ_SPURIOUS_MASTER, isr_irq_8259_spurious, 0, IDT_GATE_INTERRUPT);
    idt_set(IRQ_BASE + IRQ_SPURIOUS_SLAVE, isr_irq_8259_spurious, 0, IDT_GATE_INTERRUPT);

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

    /*
     * The lines of the machine: the I/O APIC of ADR 0012 when the firmware
     * describes one, and the 8259 of 1981 when it does not.
     *
     * The local APIC comes first: a line the I/O APIC routes is a message to
     * it, and a core whose APIC is not enabled takes nothing at all. The call
     * is the one the bring-up of the processors makes later, and it is
     * idempotent on the core that mapped the page.
     *
     * The order below is what keeps a line from arriving twice: the 8259 is
     * masked before the table of the I/O APIC is opened, and opened again if
     * the chip did not take both lines. The two chips are told the same
     * vectors (IRQ_BASE + the line), so the body of a vector does not change
     * with the controller that delivered it.
     */
    if (port_x86_64_apic_init() && port_x86_64_ioapic_init()) {
        pic_mask_all();

        if (port_x86_64_ioapic_route(IRQ_TIMER, IRQ_BASE + IRQ_TIMER, 0u) &&
            port_x86_64_ioapic_route(IRQ_KEYBOARD, IRQ_BASE + IRQ_KEYBOARD, 0u)) {
            g_ioapic_lines = true;

            /*
             * The pointer is a line of its own and a machine without a mouse
             * has nothing on it. A chip that does not take this entry is a
             * desktop without a pointer, not a machine without an interface:
             * the line stays shut and the two lines of ADR 0012 are where
             * they were.
             */
            if (!port_x86_64_ioapic_route(IRQ_MOUSE, IRQ_BASE + IRQ_MOUSE, 0u)) {
                kwarn("input: the I/O APIC did not take the line of the pointer");
            }
        } else {
            /* A chip that took one line and not the other is a chip this
             * port does not trust with either of them. */
            (void)port_x86_64_ioapic_mask(IRQ_TIMER, true);
            (void)port_x86_64_ioapic_mask(IRQ_KEYBOARD, true);
            pic_unmask_port_lines();
        }
    }

    /* The table is in the register of the processor and the lines are
     * programmed; from here the timer keeps the time of the kernel. */
    port_sti();

    /*
     * The fast door of a system call, last: the table above is the door a
     * program reaches with `int $0x80`, and this call opens the one the
     * instruction `syscall` takes (src/syscall.c). Both build the same frame
     * of 160 bytes, so a port that had one and now has two returns from
     * either the same way.
     */
    port_x86_64_syscall_init_cpu();
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

/*
 * Where the loader put the image. The BIOS loader copies the bytes to
 * PORT_X86_64_KERNEL_PHYS and the loader of a UEFI machine asks the firmware
 * for a block and says where it is; the page tables translate a pointer of
 * the image with the number this variable holds
 * (port_x86_64_kernel_phys), so it is set before the first table is built.
 */
k_u64_t g_port_x86_64_kernel_phys = PORT_X86_64_KERNEL_PHYS;

/* The boot of a UEFI machine, NULL on the BIOS path: the picture, the root of
 * the ACPI tables and the address of the image (port_x86_64_boot.h). */
static const struct port_x86_64_boot_info *g_uefi_boot;

/*
 * The memory the boot describes, and the pieces of it that are not the port's
 * to keep anything in.
 *
 * A boot loader hands the kernel a map of the memory and the modules it loaded
 * (the kernel's own bytes and the root image of a medium, ADR 0026), and the
 * port takes two things out of it: a heap for the kernel and the bytes of a
 * module a filesystem reads. Both have to live in the identity map of the
 * first gigabyte (PORT_X86_64_IDENTITY_LIMIT), and neither may be in one of
 * three places:
 *
 *  - the window of the programs, 32-48 MiB: every page table of the port, the
 *    kernel's own included, maps those 16 MiB of the address space onto the
 *    arena at 64 MiB, so a byte of the map that falls there is a byte of a
 *    program that never ran (src/paging.c). The heap of the port has always
 *    stopped below it, at `exec_base`; a module that a firmware loader puts
 *    there is a module the filesystem reads out of a program's image;
 *  - the arena itself, 64-80 MiB, where the image of the program being run is
 *    written;
 *  - the image of the kernel and the modules of the boot, which are somebody
 *    else's bytes.
 *
 * A "hole" below is one of those ranges. The two callers - the place of the
 * heap and the place of a module that has to move - ask for the first or the
 * largest run of the usable memory that avoids them.
 */
#define MEMORY_HOLES_MAX 8u
#define MEMORY_PAGE 4096u

/* The heap of the port: never smaller than a megabyte, never larger than the
 * 32 MiB the machine of 1.0 gave it. */
#define HEAP_MIN_BYTES (1024u * 1024u)
#define HEAP_MAX_BYTES (32u * 1024u * 1024u)

struct memory_hole {
    k_u64_t base;
    k_u64_t end;
};

static struct memory_hole g_holes[MEMORY_HOLES_MAX];
static k_u32_t g_hole_count;

static void holes_reset(void)
{
    g_hole_count = 0;
}

static void hole_add(k_u64_t base, k_u64_t size)
{
    if (size == 0u || g_hole_count >= MEMORY_HOLES_MAX) {
        return;
    }

    g_holes[g_hole_count].base = base;
    g_holes[g_hole_count].end = base + size;
    g_hole_count++;
}

/* The end of the first hole that the range [base, end) touches, or 0 when the
 * whole range is free. */
static k_u64_t hole_end(k_u64_t base, k_u64_t end)
{
    k_u32_t i;

    for (i = 0; i < g_hole_count; i++) {
        if (g_holes[i].base < end && base < g_holes[i].end) {
            return g_holes[i].end;
        }
    }

    return 0;
}

/* One entry of the map of the boot is memory the port may use: the format
 * names the region as usable and it starts below 4 GiB (the port reads the
 * 32-bit form of the entry). */
static bool memory_usable(const struct mb_mmap_entry *entry)
{
    return entry->type == MB_MMAP_USABLE && entry->base_high == 0u && entry->length_low != 0u;
}

/*
 * The largest run of [from, to) that no hole covers. The starts of the runs
 * are `from` and the end of every hole, and the end of a run is the beginning
 * of the next hole: with eight holes at most, a walk over those starts is the
 * whole of the search.
 */
static void memory_largest_run(k_u64_t from, k_u64_t to, k_u64_t *base, k_u64_t *size)
{
    k_u64_t starts[MEMORY_HOLES_MAX + 1u];
    k_u32_t count = 0;
    k_u32_t i;
    k_u32_t j;

    *base = 0u;
    *size = 0u;

    if (to <= from) {
        return;
    }

    starts[count++] = from;

    for (i = 0; i < g_hole_count; i++) {
        if (g_holes[i].end > from && g_holes[i].end < to) {
            starts[count++] = g_holes[i].end;
        }
    }

    for (i = 0; i < count; i++) {
        k_u64_t start = starts[i];
        k_u64_t end = to;

        /* A start inside a hole is not a start: the hole's end is one, and it
         * is in the list above. */
        if (start >= to || hole_end(start, start + 1u) != 0u) {
            continue;
        }

        for (j = 0; j < g_hole_count; j++) {
            if (g_holes[j].base > start && g_holes[j].base < end) {
                end = g_holes[j].base;
            }
        }

        if (end > start && end - start > *size) {
            *base = start;
            *size = end - start;
        }
    }
}

/*
 * The first block of `size` bytes at or above `from` that the map of the boot
 * calls usable and no hole covers, inside the identity map of the kernel. Zero
 * when the machine has no such block.
 */
static k_u64_t memory_first_fit(const struct mb_info *info, k_u64_t from, k_u64_t size)
{
    k_u64_t cursor;
    k_u64_t limit;

    if (info == NULL || (info->flags & MB_FLAG_MMAP) == 0 || info->mmap_length == 0 || size == 0u) {
        return 0u;
    }

    cursor = info->mmap_addr;
    limit = cursor + info->mmap_length;

    while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
        const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;
        k_u64_t base = entry->base_low;
        k_u64_t bytes = entry->length_low;
        k_u64_t start;
        k_u64_t end;
        k_u32_t attempt;

        cursor += entry->size + sizeof(entry->size);

        if (!memory_usable(entry)) {
            continue;
        }

        end = base + bytes;

        if (end > PORT_X86_64_IDENTITY_LIMIT) {
            end = PORT_X86_64_IDENTITY_LIMIT;
        }

        start = base > from ? base : from;
        start = (start + MEMORY_PAGE - 1u) & ~(k_u64_t)(MEMORY_PAGE - 1u);

        for (attempt = 0; attempt <= MEMORY_HOLES_MAX && start + size <= end; attempt++) {
            k_u64_t taken = hole_end(start, start + size);

            if (taken == 0u) {
                return start;
            }

            start = (taken + MEMORY_PAGE - 1u) & ~(k_u64_t)(MEMORY_PAGE - 1u);
        }
    }

    return 0u;
}

/*
 * The modules of the boot, out of the window of the programs.
 *
 * A boot loader of a medium that is not this tree's (limine, the EFI half of
 * the hybrid ISO) puts a module where its own allocator of the memory wants
 * it, and one of those places is 23.5 MiB: the 16 MB of the root image then
 * run from 23.5 to 39.5 MiB, through the window of the programs. The loader of
 * this port (loader32.S and loader/uefi) puts its modules below 20 MiB and
 * never has the problem, which is why the port could ignore the question for
 * as long as it did.
 *
 * The module is copied whole, before port_x86_64_paging_init() replaces the map
 * of the loader (that map still identity-maps the memory here, so both halves
 * of a module that straddles the window read as themselves), to the first free
 * block above the arena, and the entry of the multiboot information is written
 * with the new address: the filesystem, the heap and every reader of the module
 * then see one address, it is a physical one, and it is in the identity map.
 *
 * Module 0 - the kernel - is not moved: the loader copied it to its own bytes
 * (loader32.S) before this code ran, and nothing reads that copy again, so its
 * bytes are not even a hole of the memory.
 */
static void modules_relocate(const struct mb_info *info)
{
    const k_u64_t window_base = (k_u64_t)(uintptr_t)k_port_exec_base();
    const k_u64_t window_end = window_base + PORT_X86_64_USER_ARENA;
    const k_u64_t arena_base = PORT_X86_64_USER_PHYS;
    const k_u64_t arena_end = arena_base + PORT_X86_64_USER_ARENA;
    k_u32_t *mods;
    k_u32_t index;

    if (info == NULL || (info->flags & MB_FLAG_MODS) == 0 || info->mods_addr == 0) {
        return;
    }

    mods = (k_u32_t *)(uintptr_t)info->mods_addr;

    for (index = 1; index < info->mods_count; index++) {
        k_u32_t *entry = mods + (k_size_t)index * 4u;
        k_u64_t start = entry[0];
        k_u64_t end = entry[1];
        k_u64_t size;
        k_u64_t target;

        if (end <= start) {
            continue;
        }

        size = end - start;

        if (!(start < window_end && window_base < end) &&
            !(start < arena_end && arena_base < end)) {
            /* The module is where the port may keep it: it is a hole for the
             * heap and it stays there. */
            hole_add(start, size);
            continue;
        }

        target = memory_first_fit(info, arena_end, size);

        if (target == 0u) {
            kwarn("x86-64: the module %u lies in the window of the programs and has nowhere to go",
                  (unsigned)index);
            hole_add(start, size);
            continue;
        }

        memcpy((void *)(uintptr_t)target, (const void *)(uintptr_t)start, (k_size_t)size);
        hole_add(target, size);

        entry[0] = (k_u32_t)target;
        entry[1] = (k_u32_t)(target + size);

        kinfo("x86-64: the module %u (%u KB) was in the window of the programs, moved to 0x%x",
              (unsigned)index, (unsigned)(size / 1024u), (unsigned)target);
    }
}

/* The holes of the memory and the modules out of them, once, before the heap
 * and before the page tables of the port: both of them are asked here. */
static void memory_bring_up(const struct mb_info *info)
{
    const k_u64_t window_base = (k_u64_t)(uintptr_t)k_port_exec_base();
    const k_u64_t kernel_end = port_x86_64_kernel_phys(__kernel_end);

    holes_reset();

    /* The image of the kernel, and then the modules: a module that has to move
     * must not be moved onto the kernel or onto another module. */
    hole_add(g_port_x86_64_kernel_phys, kernel_end - g_port_x86_64_kernel_phys);
    modules_relocate(info);

    hole_add(window_base, PORT_X86_64_USER_ARENA);
    hole_add(PORT_X86_64_USER_PHYS, PORT_X86_64_USER_ARENA);
}

/* The largest run of the memory of the boot inside [from, to) that no hole
 * covers: the pieces of every usable entry of the map, and the largest of them
 * all. */
static void heap_take(const struct mb_info *info, k_u64_t from, k_u64_t to, k_u64_t *base,
                      k_u64_t *size)
{
    k_u64_t cursor;
    k_u64_t limit;

    *base = 0u;
    *size = 0u;

    cursor = info->mmap_addr;
    limit = cursor + info->mmap_length;

    while (cursor + sizeof(struct mb_mmap_entry) <= limit) {
        const struct mb_mmap_entry *entry = (const struct mb_mmap_entry *)(uintptr_t)cursor;
        k_u64_t start = entry->base_low;
        k_u64_t end = start + entry->length_low;
        k_u64_t piece_base;
        k_u64_t piece_size;

        cursor += entry->size + sizeof(entry->size);

        if (!memory_usable(entry)) {
            continue;
        }

        if (start < from) {
            start = from;
        }

        if (end > to) {
            end = to;
        }

        if (end > PORT_X86_64_IDENTITY_LIMIT) {
            end = PORT_X86_64_IDENTITY_LIMIT;
        }

        memory_largest_run(start, end, &piece_base, &piece_size);

        if (piece_size > *size) {
            *base = piece_base;
            *size = piece_size;
        }
    }
}

/*
 * The heap of the port: the biggest piece of the memory that is the kernel's,
 * in three places in the order of their preference.
 *
 *  1. below the window of the programs, between the image of the kernel and
 *     32 MiB. Every boot of this port until the hybrid ISO had its heap here
 *     (16.6-32 MiB with the modules of the loader of the port);
 *  2. the 16 MiB between the window and the arena, 48-64 MiB, for a boot whose
 *     modules left nothing below the window;
 *  3. above the arena, below the map of the kernel, which is the memory of a
 *     machine whose every megabyte below 64 MiB is taken.
 *
 * The heap of a machine is a megabyte at the least and 32 MiB at the most, and
 * a machine that has neither piece has no heap: the boot says so and the kernel
 * runs on its own bytes.
 */
static void heap_init(const struct mb_info *info)
{
    const k_u64_t window_base = (k_u64_t)(uintptr_t)k_port_exec_base();
    const k_u64_t window_end = window_base + PORT_X86_64_USER_ARENA;
    const k_u64_t arena_base = PORT_X86_64_USER_PHYS;
    const k_u64_t arena_end = arena_base + PORT_X86_64_USER_ARENA;
    k_u64_t kernel_end = port_x86_64_kernel_phys(__kernel_end);
    k_u64_t base = 0u;
    k_u64_t size = 0u;

    if (info == NULL || (info->flags & MB_FLAG_MMAP) == 0 || info->mmap_length == 0) {
        return;
    }

    heap_take(info, kernel_end, window_base, &base, &size);

    if (size < HEAP_MIN_BYTES) {
        heap_take(info, window_end, arena_base, &base, &size);
    }

    if (size < HEAP_MIN_BYTES) {
        heap_take(info, arena_end, PORT_X86_64_IDENTITY_LIMIT, &base, &size);
    }

    if (size > HEAP_MAX_BYTES) {
        size = HEAP_MAX_BYTES;
    }

    if (size < HEAP_MIN_BYTES) {
        kwarn("x86-64: the memory of the boot has no megabyte for the heap of the kernel");
        return;
    }

    base = (base + BLOCK_ALIGN - 1) & ~(k_u64_t)(BLOCK_ALIGN - 1);

    g_first = (struct block *)(uintptr_t)base;
    g_first->size = size - sizeof(struct block);
    g_first->next = NULL;
    g_first->free = true;

    g_heap_total = (k_size_t)g_first->size;
    g_heap_free = g_heap_total;
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
#define PORT_API_MINOR 12u

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

/* --- the clock of the firmware: the CMOS chip of the machine ---------------
 *
 * Every machine of this port has the battery-backed chip the firmware reads
 * its setup from, and the same chip keeps the date: the seconds, the minutes,
 * the hours, the day, the month and the year in BCD behind one index port and
 * one data port. The kernel keeps its own wall clock (kernel/clock.h) and the
 * chip is what seeds it at the boot, so a laptop that has been off since last
 * night knows the date when the shell comes up and not the seconds since its
 * own start.
 *
 * The registers are read twice and compared: the chip updates them every
 * second from its own oscillator, and a read that straddles the update is a
 * moment that never existed (23:59:59 of the 31st and 00:00:01 of the 1st).
 * The NMI of the machine is masked while the index port is written, which is
 * the convention of the chip and keeps a byte of the machine's memory from
 * being read instead.
 */
#define CMOS_INDEX 0x70u
#define CMOS_DATA 0x71u

#define CMOS_SECOND 0x00u
#define CMOS_MINUTE 0x02u
#define CMOS_HOUR 0x04u
#define CMOS_DAY 0x07u
#define CMOS_MONTH 0x08u
#define CMOS_YEAR 0x09u
#define CMOS_CENTURY 0x32u
#define CMOS_STATUS_A 0x0Au
#define CMOS_STATUS_B 0x0Bu

#define CMOS_BINARY 0x04u /* the registers are binary instead of BCD */
#define CMOS_24_HOUR 0x02u
#define CMOS_PM 0x80u     /* the hours register in the 12-hour mode */
#define CMOS_SET 0x80u    /* the updates are stopped, so the registers are ours */
#define CMOS_UPDATE 0x80u /* the bit of the status register: an update is coming */

static k_u8_t cmos_read(k_u8_t reg)
{
    port_outb(CMOS_INDEX, (k_u8_t)(reg | 0x80u));
    return port_inb(CMOS_DATA);
}

static void cmos_write(k_u8_t reg, k_u8_t value)
{
    port_outb(CMOS_INDEX, (k_u8_t)(reg | 0x80u));
    port_outb(CMOS_DATA, value);
}

/* Wait for the chip to be between two updates; false when it never is. */
static bool cmos_idle(void)
{
    int attempt;

    for (attempt = 0; attempt < 1000000; attempt++) {
        if ((cmos_read(CMOS_STATUS_A) & CMOS_UPDATE) == 0) {
            return true;
        }
    }

    return false;
}

static int cmos_to_binary(k_u8_t value, bool binary)
{
    return binary ? (int)value : ((int)(value >> 4) * 10 + (int)(value & 0x0Fu));
}

static k_u8_t cmos_to_register(int value, bool binary)
{
    if (binary) {
        return (k_u8_t)value;
    }

    return (k_u8_t)(((value / 10) << 4) | (value % 10));
}

/* The day count of a date from 1970-01-01, both ways (Hinnant's calendar). */
static k_i64_t cmos_days_from_civil(int year, int month, int day)
{
    k_i64_t y = (k_i64_t)year - ((month <= 2) ? 1 : 0);
    k_i64_t era = (y >= 0 ? y : y - 399) / 400;
    k_u32_t yoe = (k_u32_t)(y - era * 400);
    k_u32_t doy = (k_u32_t)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    k_u32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (k_i64_t)doe - 719468;
}

static void cmos_civil_from_days(k_i64_t days, int *year, int *month, int *day)
{
    k_i64_t z = days + 719468;
    k_i64_t era = (z >= 0 ? z : z - 146096) / 146097;
    k_u32_t doe = (k_u32_t)(z - era * 146097);
    k_u32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    k_u32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    k_u32_t mp = (5 * doy + 2) / 153;
    int m = (int)mp + (mp < 10 ? 3 : -9);

    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = m;
    *year = (int)((k_i64_t)yoe + era * 400) + ((m <= 2) ? 1 : 0);
}

/*
 * The moment the chip holds, in the seconds of Unix (UTC). False when the
 * chip does not answer or holds a moment that is not a date: a dead battery
 * leaves fields like 58 for an hour, and a number that is not a date is worse
 * than no clock at all.
 */
static bool cmos_read_unix(k_u64_t *out)
{
    k_u8_t second = 0;
    k_u8_t minute = 0;
    k_u8_t hour = 0;
    k_u8_t day = 0;
    k_u8_t month = 0;
    k_u8_t year = 0;
    k_u8_t century = 0x20u;
    k_u8_t status = 0;
    bool binary;
    bool stable = false;
    int attempt;
    int s;
    int m;
    int h;
    int y;
    int days;

    for (attempt = 0; attempt < 8 && !stable; attempt++) {
        if (!cmos_idle()) {
            return false;
        }

        second = cmos_read(CMOS_SECOND);
        minute = cmos_read(CMOS_MINUTE);
        hour = cmos_read(CMOS_HOUR);
        day = cmos_read(CMOS_DAY);
        month = cmos_read(CMOS_MONTH);
        year = cmos_read(CMOS_YEAR);
        century = cmos_read(CMOS_CENTURY);
        status = cmos_read(CMOS_STATUS_B);

        /* The moment is one moment only when the seconds did not move under
         * the read. The month is the register of the bit of the century, and
         * a chip whose month is 0 is not one of this machine. */
        stable = (second == cmos_read(CMOS_SECOND)) && ((month & 0x1Fu) != 0);
    }

    if (!stable) {
        return false;
    }

    binary = (status & CMOS_BINARY) != 0;

    s = cmos_to_binary(second, binary);
    m = cmos_to_binary(minute, binary);
    h = cmos_to_binary((k_u8_t)(hour & 0x7Fu), binary);
    days = cmos_to_binary(day, binary);
    month = (k_u8_t)cmos_to_binary((k_u8_t)(month & 0x1Fu), binary);
    year = (k_u8_t)cmos_to_binary(year, binary);

    if ((status & CMOS_24_HOUR) == 0) {
        /* The 12-hour mode: the high bit is the afternoon, and 12:xx is the
         * hour zero of it. */
        if (h == 12) {
            h = 0;
        }
        if ((hour & CMOS_PM) != 0) {
            h += 12;
        }
    }

    y = cmos_to_binary(year, binary);

    {
        int century_value = cmos_to_binary(century, binary);

        /*
         * The century is an addition of ACPI; a chip of an older machine
         * keeps something else in that register. A value that is not 19 or 20
         * is not a century, and the year decides: 70 to 99 belongs to the
         * last one and 00 to 69 to this one.
         */
        if (century_value != 19 && century_value != 20) {
            century_value = (y >= 70) ? 19 : 20;
        }

        y += century_value * 100;
    }

    if (y >= 1900 && y <= 2099 && month >= 1 && month <= 12 && days >= 1 && days <= 31 && h >= 0 &&
        h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 59) {
        *out = (k_u64_t)(cmos_days_from_civil(y, month, days) * 86400 + h * 3600 + m * 60 + s);

        return true;
    }

    return false;
}

/*
 * The setter the kernel keeps for the clock (kernel/clock.h): a moment that
 * came from NTP goes back into the chip, so the next boot of the machine
 * starts from it. The write is the plain one of the chip - the update is
 * stopped for the seven registers, the mode is BCD and 24 hours - and a chip
 * that refuses the write is not a refusal of the call: the kernel keeps the
 * moment in its own base and the next boot falls back to the chip it has.
 */
static int cmos_clock_set(k_u64_t unix_seconds)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int days;
    k_u64_t held = 0;
    k_u8_t status;

    if (unix_seconds > 4102444799u) { /* past 2099-12-31, the chip's calendar */
        return -1;
    }

    days = (int)((k_i64_t)(unix_seconds / 86400u));
    second = (int)(unix_seconds % 60u);
    minute = (int)((unix_seconds / 60u) % 60u);
    hour = (int)((unix_seconds / 3600u) % 24u);

    cmos_civil_from_days(days, &year, &month, &day);

    if (year < 2000) {
        return -1;
    }

    /* The moment is already in the chip: the boot read it and set it. */
    if (cmos_read_unix(&held) && held == unix_seconds) {
        return 0;
    }

    if (!cmos_idle()) {
        return 0;
    }

    status = cmos_read(CMOS_STATUS_B);
    cmos_write(CMOS_STATUS_B, (k_u8_t)((status | CMOS_24_HOUR | CMOS_SET) & ~CMOS_BINARY));
    cmos_write(CMOS_SECOND, cmos_to_register(second, false));
    cmos_write(CMOS_MINUTE, cmos_to_register(minute, false));
    cmos_write(CMOS_HOUR, cmos_to_register(hour, false));
    cmos_write(CMOS_DAY, cmos_to_register(day, false));
    cmos_write(CMOS_MONTH, cmos_to_register(month, false));
    cmos_write(CMOS_YEAR, cmos_to_register(year % 100, false));

    /*
     * The century register is written only when it already holds a century:
     * a chip of a machine older than ACPI keeps a byte of the firmware there,
     * and a year of two digits is still the year of this machine.
     */
    {
        k_u8_t century = cmos_read(CMOS_CENTURY);

        if (century == 0x19u || century == 0x20u) {
            cmos_write(CMOS_CENTURY, cmos_to_register(year / 100, false));
        }
    }

    cmos_write(CMOS_STATUS_B, status);

    return 0;
}

/* The clock of the machine, read once at the boot and kept by the kernel. */
static void cmos_clock_seed(void)
{
    k_u64_t now = 0;

    if (!cmos_read_unix(&now)) {
        kwarn("x86-64: the CMOS chip holds no date, the clock starts at the boot");
        return;
    }

    (void)k_clock_set_unix(now);
}

/* --- the init of the port --- */

static struct port_x86_64_info g_info;

void port_x86_64_init(k_u64_t boot_info, k_u64_t boot_magic, k_u64_t kernel_phys)
{
    const struct mb_info *info;

    /*
     * Where the loader put the image, before anything translates an address
     * of the kernel: the bytes of the image are at the top of the address
     * space and at this number of the memory, and both are needed for a page
     * table entry as soon as the first one is built. Zero means the loader
     * did not say and the address of the header is the one.
     */
    if (kernel_phys >= 0x100000ull) {
        g_port_x86_64_kernel_phys = kernel_phys;
    }

    /*
     * The two boots of the port are told apart by the second number the entry
     * was given: the loader of the BIOS path leaves it zero and hands the
     * multiboot information, and the loader of a UEFI machine puts the magic
     * of the structure of port_x86_64_boot.h there. The multiboot information
     * travels inside that structure, so the heap, the modules and the map of
     * the memory are read by the same code on both machines.
     */
    if (boot_magic == PORT_X86_64_BOOT_MAGIC && boot_info != 0u) {
        g_uefi_boot = (const struct port_x86_64_boot_info *)(uintptr_t)boot_info;

        if (g_uefi_boot->version != PORT_X86_64_BOOT_VERSION) {
            /* A structure of a loader of another release: the port reads the
             * part of it the two versions share and says so. */
            kerror("x86-64: the boot is version %u of the hand-off, the port reads %u",
                   (unsigned)g_uefi_boot->version, (unsigned)PORT_X86_64_BOOT_VERSION);
        }

        info = (const struct mb_info *)(uintptr_t)g_uefi_boot->mb_info;
    } else {
        g_uefi_boot = NULL;
        info = (const struct mb_info *)(uintptr_t)boot_info;
    }

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
    k_clock_register(cmos_clock_set);
    cmos_clock_seed();
    port_x86_64_cpus_init();
    /*
     * The memory of the machine, before the page tables of the port: a module
     * that a loader of the medium put inside the window of the programs is
     * copied out of it while the identity map of the loader still names every
     * byte of it, and the heap of the kernel is placed after the modules and
     * out of the window.
     */
    memory_bring_up(info);
    heap_init(info);
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
    /* 1 is the multiboot 1 information of the BIOS path, and 2 says that the
     * numbers of the boot came from the firmware through the hand-off of a
     * UEFI machine (the `boot_version` of the contract is what a reader of
     * this port's log calls "the version of the boot"). */
    g_info.boot_version = info != NULL ? (g_uefi_boot != NULL ? 2u : 1u) : 0u;
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
    port_x86_64_virtio_blk_init();
    port_x86_64_ahci_init();

    /* The network is the last device of the boot before the interrupts: it
     * asks the PCI bus for a card, and a machine without one keeps the "no"
     * answers of the contract. */
    port_x86_64_net_init();

    /* Last, because it is the point of no return: from here the timer fires
     * every millisecond and the kernel has to be able to take it. */
    port_x86_64_interrupts_init();

    /* The two disks asked for their lines before the table existed: the
     * drivers open them now, and the bus master of the channel and the queue
     * of the card become the ones the transfers use (ADR 0032, ADR 0037). */
    port_x86_64_disk_irq_init();
    port_x86_64_virtio_blk_irq_init();

    /*
     * The keyboard of the USB (ADR 0040). The controller is optional and the
     * machine of a laptop is the one that needs it: the wait of the
     * enumeration is measured by the clock, so the call lives after the
     * timer, and the PS/2 of the machine keeps working beside it.
     */
    port_x86_64_xhci_init();

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

const struct port_x86_64_boot_info *port_x86_64_boot(void)
{
    return g_uefi_boot;
}

const char *port_x86_64_boot_name(void)
{
    return g_uefi_boot != NULL ? "uefi" : "multiboot";
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
