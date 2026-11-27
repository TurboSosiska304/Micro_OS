/*
 * Micro OS — the application processors of the x86-64 machine (ADR 0012).
 *
 * This file is the owner of the bring-up: the processor of the boot reads the
 * list of the firmware, gives every core a stack, writes the parameters of
 * the trampoline at physical 0x8000, sends INIT and the start-up IPI through
 * the local APIC, and waits for the core to answer from C. The core then
 * stops before the scheduler until kmain() releases it, because the array of
 * the cores of the kernel (kernel/cpu.c) does not exist while the port is
 * still being brought up.
 *
 * Three rules shape the code below.
 *
 * First, nothing here pretends. A core counts when it reached C and was
 * accepted, and the boot prints the number that did; a core the firmware
 * lists and the IPI never woke is a missing core with a line of its own, not
 * a number in the count the scheduler works from.
 *
 * Second, a core that is not accepted into the kernel never touches the
 * scheduler. It halts on its own stack and stays there: two cores in one run
 * queue array is the failure this whole exercise is about, and a core that
 * was started but not counted is the one place it could happen.
 *
 * Third, a failure of one core never resets the machine. The signature of the
 * trampoline is checked before a SIPI is sent: an AP that executes the bytes
 * a firmware left at 0x7000 can write to a reset port, and the whole desktop
 * goes back to the firmware instead of reaching a shell that could say why.
 * Every phase of the sequence is printed as it happens - to the screen and to
 * COM1 - and repeated in the log once the kernel has booted.
 */
#include "port_x86_64.h"

#include <stdio.h>
#include <string.h>

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port/console.h"
#include "kernel/port/cpu.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

#include "port_x86_64_apic.h"
#include "port_x86_64_smp.h"
#include "port_x86_64_task.h"

/* The cores that reached C and answered the start-up IPI. One is the boot. */
static k_u32_t g_checked_in = 1;

#if KERNEL_SMP

/* --- the numbers of the bring-up ------------------------------------------ */

/*
 * The start-up IPI names a page of 4 KiB: vector 7 is 0x7000, where the
 * trampoline lives (src/ap_trampoline.S), and the parameters live on the next
 * page, 0x8000, writable and NX in the map of the kernel. The signature at
 * 0x7F00 is written into the blob and copied with it; a page without it is
 * not code the bring-up is willing to run on another processor. Both
 * addresses are physical and the first gigabyte is identity mapped, so a
 * pointer here is the address.
 */
#define AP_TRAMPOLINE_VECTOR 7u
#define AP_TRAMPOLINE_PHYS 0x7000u
#define AP_SIGNATURE_PHYS 0x7F00u
#define AP_PARAM_PHYS 0x8000u

/* The signature is eight bytes of the trampoline, including the two zeros. */
static const k_u8_t g_ap_signature[8] = {'M', 'O', 'S', 'A', 'P', '1', 0, 0};

/* The same blob is linked into the image so that a firmware loader, which
 * does not carry it, can still give an AP a way into long mode. */
extern const k_u8_t port_x86_64_ap_trampoline_image[];
extern const k_u8_t port_x86_64_ap_trampoline_image_end[];

/* The stack of a core lasts the life of the machine: nothing frees it. It is
 * larger than the stack of a task because the idle loop of a core and its
 * interrupts share it, and a bring-up that ran out of stack would be a triple
 * fault with no line. */
#define AP_STACK_BYTES (64u * 1024u)

/* The waits of the sequence, in microseconds. The architecture asks ten
 * milliseconds between INIT and the start-up IPI, two hundred microseconds
 * between the two start-ups, and gives a processor that much time again to
 * begin executing; the bound here is generous because a virtual machine
 * under load is slower than the processor it imitates. */
#define AP_IPI_TIMEOUT_US 10000u
#define AP_AFTER_INIT_US 10000u
#define AP_SIPI_GAP_US 200u
#define AP_START_WAIT_US 100000u

/* The phases a core can be in. They are also the names in the lines that go
 * to the screen during the bring-up and to `dmesg` after it. */
enum ap_phase {
    AP_PHASE_NONE = 0u,
    AP_PHASE_TRAMPOLINE,
    AP_PHASE_PARAMS,
    AP_PHASE_INIT,
    AP_PHASE_AFTER_INIT,
    AP_PHASE_SIPI1,
    AP_PHASE_GAP,
    AP_PHASE_SIPI2,
    AP_PHASE_IN_C,
    AP_PHASE_ACCEPTED,
    AP_PHASE_COUNT
};

static const char *const g_phase_names[AP_PHASE_COUNT] = {
    "not started", "trampoline signature", "parameters", "INIT", "10 ms after INIT",
    "SIPI 1",      "200 us gap",           "SIPI 2",     "in C", "accepted"};

static const char *const g_icr_step_names[] = {"ok", "INIT assert", "INIT deassert", "SIPI", "IPI"};

struct ap_bringup {
    k_u64_t start_tsc;
    k_u32_t apic_id;
    k_u32_t seen_id;
    k_u32_t phase;
    k_u32_t waited_us;
    k_u32_t elapsed_us;
    bool ok;
};

/* One record per core, written by the boot while the cores are brought up one
 * at a time. The AP writes only its own identifier and the `reached` flag. */
static struct ap_bringup g_ap_bringup[KERNEL_CPUS_MAX];

/* The lines of the bring-up, kept so the log of the kernel can say them again
 * once kernel_boot() has made its ring. A hundred is more than eight cores
 * need for the nine phases of the sequence. */
#define AP_LOG_LINES 100u
#define AP_LOG_LINE_MAX 128u
static char g_ap_log[AP_LOG_LINES][AP_LOG_LINE_MAX];
static k_u8_t g_ap_log_cpu[AP_LOG_LINES];
static k_u32_t g_ap_log_count;
static k_u32_t g_trampoline_source; /* 0 none, 1 loader, 2 kernel image */
static bool g_trampoline_ok;

/* What the bring-up learned about one core. The bits are the ones of
 * port_x86_64_smp.h; the writer is the core itself until the boot accepts
 * it, and the boot from then on. */
static bool g_reached[KERNEL_CPUS_MAX];
static bool g_online[KERNEL_CPUS_MAX];
static bool g_apic_ok[KERNEL_CPUS_MAX];
static bool g_timer_ok[KERNEL_CPUS_MAX];

/* The flag that lets the cores that answered into the scheduler. */
static bool g_released;

/* The stack of every core, and the top the trampoline is given. The block is
 * kept here for the life of the machine, and the top is what the C of the
 * core reads before the first switch: a static array written before the SIPI
 * is the only place both halves can see. */
static k_u8_t *g_ap_stacks[KERNEL_CPUS_MAX];
static k_u64_t g_ap_stack_top[KERNEL_CPUS_MAX];

/* The cores the firmware listed that never reached C. */
static k_u32_t g_failures;

/* The lock of the lines of the `smp` command. The console of this port is one
 * resource guarded per core, not per machine (k_port_write turns the
 * interrupts of the caller off), so two cores that print at once put the
 * bytes of one line between two of the other. The command and its workers
 * take this mutex, which sleeps instead of holding the scheduler lock across
 * a write of the UART. */
static k_port_mutex_t *g_print_lock;

/* --- the clock of the waits ------------------------------------------------- */

/*
 * Wait for `us` microseconds on the TSC. The frequency is the one the boot
 * measured against the PIT; a machine whose measurement failed has no
 * trustworthy TSC, and the fallback is a counted loop of pauses that is
 * slower than the time asked for and still terminates.
 */
static void tsc_delay(k_u32_t us)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t ticks = 0;
    k_u64_t start;

    if (info != NULL && info->cpu_hz != 0u) {
        ticks = (info->cpu_hz / 1000000u) * (k_u64_t)us;
    }

    if (ticks == 0u) {
        volatile k_u32_t spin;
        k_u32_t budget = us * 100u;

        for (spin = 0; spin < budget; spin++) {
            __asm__ volatile("pause");
        }

        return;
    }

    start = port_rdtsc();

    while (port_rdtsc() - start < ticks) {
        __asm__ volatile("pause");
    }
}

/* Wait for a flag a core writes, up to `us` microseconds. False when the time
 * ran out and the flag is still down. */
static bool wait_flag(bool *flag, k_u32_t us)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t ticks = 0;

    if (info != NULL && info->cpu_hz != 0u) {
        ticks = (info->cpu_hz / 1000000u) * (k_u64_t)us;
    }

    if (ticks == 0u) {
        volatile k_u32_t spin;
        k_u32_t budget = us * 100u;

        for (spin = 0; spin < budget; spin++) {
            if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
                return true;
            }

            __asm__ volatile("pause");
        }

        return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
    }

    {
        k_u64_t start = port_rdtsc();

        while (port_rdtsc() - start < ticks) {
            if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
                return true;
            }

            __asm__ volatile("pause");
        }
    }

    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

/* --- what the boot says about each phase ------------------------------------ */

/* Keep a line for `dmesg` and put it on the screen and COM1 now. Before the
 * console exists the caller writes the same line straight to COM1; the ring
 * of the kernel replays it once kernel_boot() has run. */
static void ap_emit(const char *line, bool live, k_u32_t cpu)
{
    k_size_t len = strlen(line);

    if (len >= AP_LOG_LINE_MAX) {
        len = AP_LOG_LINE_MAX - 1u;
    }

    if (g_ap_log_count < AP_LOG_LINES) {
        memcpy(g_ap_log[g_ap_log_count], line, len);
        g_ap_log[g_ap_log_count][len] = '\0';
        g_ap_log_cpu[g_ap_log_count] = (k_u8_t)cpu;
        g_ap_log_count++;
    }

    if (live) {
        k_port_write(line, len);
        k_port_write("\n", 1u);
    }
}

/* Microseconds since the start of one core, measured by the clock of the boot. */
static k_u32_t ap_us_since(k_u64_t start)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t hz;

    if (info == NULL || info->cpu_hz == 0u) {
        return 0u;
    }

    hz = info->cpu_hz;
    return (k_u32_t)(((port_rdtsc() - start) * 1000000u) / hz);
}

static bool ap_signature_ok(void)
{
    return memcmp((const void *)(uintptr_t)AP_SIGNATURE_PHYS, g_ap_signature,
                  sizeof(g_ap_signature)) == 0;
}

/*
 * Make sure the page vector 7 names really holds the trampoline. The BIOS
 * loader copies it there before the kernel runs; a firmware loader does not,
 * and a SIPI to a page of firmware leftovers can end in a write to a reset
 * port on real metal. The copy happens while the map of the loader is still
 * writable and before the kernel marks the page executable and read-only.
 */
void port_x86_64_ap_trampoline_install(void)
{
    const k_u8_t *image = port_x86_64_ap_trampoline_image;
    k_size_t bytes = (k_size_t)(port_x86_64_ap_trampoline_image_end - image);
    char line[AP_LOG_LINE_MAX];
    int n;

    /* The blob also carries the zeroed parameter block on the next page. */
    if (bytes == 0u || bytes > 8192u) {
        g_trampoline_ok = false;
        n = snprintf(line, sizeof(line),
                     "x86-64: ap trampoline: the image has %u byte(s), cannot install",
                     (unsigned)bytes);
    } else if (ap_signature_ok()) {
        g_trampoline_source = 1u;
        g_trampoline_ok = true;
        n = snprintf(line, sizeof(line),
                     "x86-64: ap trampoline at 0x7000 from loader, signature ok, boot %s",
                     port_x86_64_boot_name());
    } else {
        memcpy((void *)(uintptr_t)AP_TRAMPOLINE_PHYS, image, bytes);
        g_trampoline_source = 2u;
        g_trampoline_ok = ap_signature_ok();
        n = snprintf(line, sizeof(line),
                     "x86-64: ap trampoline at 0x7000 from kernel image, signature %s, boot %s",
                     g_trampoline_ok ? "ok" : "missing", port_x86_64_boot_name());
    }

    if (n > 0) {
        /* The framebuffer console is not up yet on this path. */
        ap_emit(line, false, KERNEL_CPUS_MAX);
        port_x86_64_debug_write(line, (k_size_t)n);
        port_x86_64_debug_write("\n", 1u);
    }
}

/* One line per phase, on the screen and in the boot log of the kernel. */
static void ap_phase(k_u32_t cpu, enum ap_phase phase, k_u32_t limit_us, bool ok)
{
    struct ap_bringup *record = &g_ap_bringup[cpu];
    char line[AP_LOG_LINE_MAX];
    int n;

    record->phase = phase;
    record->waited_us = limit_us;
    record->elapsed_us = ap_us_since(record->start_tsc);
    record->ok = ok;

    n = snprintf(line, sizeof(line), "x86-64: ap cpu %u apic 0x%x: %s %s, %u us of %u us",
                 (unsigned)cpu, (unsigned)record->apic_id, g_phase_names[phase],
                 ok ? "ok" : "failed", record->elapsed_us, limit_us);

    if (n > 0) {
        ap_emit(line, true, cpu);
    }
}

/* A command stuck in the local APIC: say which one and how long it waited. */
static void ap_icr_failure(k_u32_t cpu, enum ap_phase phase)
{
    k_u32_t step = 0u;
    k_u32_t waited_us = 0u;
    char line[AP_LOG_LINE_MAX];
    int n;

    port_x86_64_apic_last_icr(&step, &waited_us);
    n = snprintf(
        line, sizeof(line), "x86-64: ap cpu %u apic 0x%x: ICR %s did not finish, waited %u us",
        (unsigned)cpu, (unsigned)g_ap_bringup[cpu].apic_id,
        step < sizeof(g_icr_step_names) / sizeof(g_icr_step_names[0]) ? g_icr_step_names[step]
                                                                      : "command",
        waited_us);

    if (n > 0) {
        ap_emit(line, true, cpu);
    }

    ap_phase(cpu, phase, AP_IPI_TIMEOUT_US, false);
}

/* --- starting one core ------------------------------------------------------ */

/*
 * Write the parameters of the trampoline. They are four 64-bit words and the
 * AP reads them before it has a stack, so they are volatile stores in the
 * order the trampoline expects: the page tables, the stack, the C entry and
 * the number of the core. The last one is the identity the core needs before
 * any kernel call.
 */
static void ap_params_write(k_u32_t cpu)
{
    volatile k_u64_t *params = (volatile k_u64_t *)(uintptr_t)AP_PARAM_PHYS;

    params[0] = port_x86_64_kernel_cr3();
    params[1] = g_ap_stack_top[cpu];
    params[2] = (k_u64_t)(uintptr_t)port_x86_64_ap_entry;
    params[3] = (k_u64_t)cpu;
}

/*
 * Give one core a stack and send it the sequence of the PC: INIT, ten
 * milliseconds, the start-up IPI, two hundred microseconds, the start-up IPI
 * again. Every phase writes one line before the next begins, so a machine
 * that stops in the middle of the sequence says where it stopped. The answer
 * is false when a command did not leave the local APIC, the trampoline has no
 * signature, or the core did not reach C in time; the caller counts it and
 * goes on to the next one, because one silent core must not cost the others.
 */
static bool ap_start(k_u32_t cpu)
{
    struct ap_bringup *record = &g_ap_bringup[cpu];
    k_u8_t *block;
    k_u32_t apic_id = record->apic_id;
    bool reached;

    record->start_tsc = port_rdtsc();
    record->seen_id = 0u;
    record->ok = false;

    /*
     * The page a SIPI names has to be ours. Without this check an AP of a
     * firmware boot executes whatever was at 0x7000, and on real metal that
     * can reach a port that resets the whole machine before a line is read.
     */
    if (!g_trampoline_ok || !ap_signature_ok()) {
        kinfo("x86-64: ap cpu %u apic 0x%x: no trampoline signature, SIPI not sent", (unsigned)cpu,
              (unsigned)apic_id);
        ap_phase(cpu, AP_PHASE_TRAMPOLINE, 0u, false);
        return false;
    }

    ap_phase(cpu, AP_PHASE_TRAMPOLINE, 0u, true);

    block = (k_u8_t *)k_port_alloc(AP_STACK_BYTES, false);

    if (block == NULL) {
        kinfo("x86-64: ap cpu %u apic 0x%x: no 64 KiB stack", (unsigned)cpu, (unsigned)apic_id);
        ap_phase(cpu, AP_PHASE_PARAMS, 0u, false);
        return false;
    }

    /* A stack that has never been used reads better as zeros in a dump. */
    memset(block, 0, AP_STACK_BYTES);

    g_ap_stacks[cpu] = block;
    g_ap_stack_top[cpu] = ((k_u64_t)(uintptr_t)block + AP_STACK_BYTES) & ~(k_u64_t)15u;

    ap_params_write(cpu);
    ap_phase(cpu, AP_PHASE_PARAMS, 0u, true);

    if (!port_x86_64_apic_send_init(apic_id, AP_IPI_TIMEOUT_US)) {
        ap_icr_failure(cpu, AP_PHASE_INIT);
        return false;
    }
    ap_phase(cpu, AP_PHASE_INIT, AP_IPI_TIMEOUT_US, true);

    tsc_delay(AP_AFTER_INIT_US);
    ap_phase(cpu, AP_PHASE_AFTER_INIT, AP_AFTER_INIT_US, true);

    if (!port_x86_64_apic_send_startup(apic_id, AP_TRAMPOLINE_VECTOR, AP_IPI_TIMEOUT_US)) {
        ap_icr_failure(cpu, AP_PHASE_SIPI1);
        return false;
    }
    ap_phase(cpu, AP_PHASE_SIPI1, AP_IPI_TIMEOUT_US, true);

    tsc_delay(AP_SIPI_GAP_US);
    ap_phase(cpu, AP_PHASE_GAP, AP_SIPI_GAP_US, true);

    if (!port_x86_64_apic_send_startup(apic_id, AP_TRAMPOLINE_VECTOR, AP_IPI_TIMEOUT_US)) {
        ap_icr_failure(cpu, AP_PHASE_SIPI2);
        return false;
    }
    ap_phase(cpu, AP_PHASE_SIPI2, AP_IPI_TIMEOUT_US, true);

    reached = wait_flag(&g_reached[cpu], AP_START_WAIT_US);
    ap_phase(cpu, AP_PHASE_IN_C, AP_START_WAIT_US, reached);

    return reached;
}

/*
 * The bring-up of the machine. Called at the end of port_x86_64_init(), after
 * the page tables and the table of the interrupts: the core that is started
 * has to find both, and it enters C with the interrupts off and the map of
 * the kernel.
 */
void port_x86_64_smp_init(void)
{
    k_u32_t available;
    k_u32_t count;
    k_u32_t cpu;
    k_u32_t started = 1;

    available = port_x86_64_cpu_available();
    count = (available < KERNEL_CPUS_MAX) ? available : KERNEL_CPUS_MAX;

    /* A record for every core the firmware named, even the ones that are
     * never sent an IPI: `smp` and `dmesg` name them all afterwards. */
    for (cpu = 0u; cpu < count; cpu++) {
        g_ap_bringup[cpu].apic_id = port_x86_64_cpu_apic_id(cpu);
        g_ap_bringup[cpu].phase = AP_PHASE_NONE;
    }

    if (!port_x86_64_apic_init()) {
        /*
         * A machine without a local APIC has no way to send INIT or a
         * start-up IPI: it runs on the processor of the boot and the boot
         * says so. The cores of the firmware are all left idle.
         */
        kinfo("x86-64: no local APIC, %u core(s) left idle", (unsigned)(count - 1u));
        g_failures = (available > 1u) ? available - 1u : 0u;
        g_checked_in = 1;
        return;
    }

    /*
     * One core at a time, and the parameters written before each start: the
     * block at 0x8000 is one page of the machine and two cores that read it
     * at once would take the stack of the other.
     */
    for (cpu = 1; cpu < count; cpu++) {
        k_u32_t seen;

        if (!ap_start(cpu)) {
            continue;
        }

        /*
         * The processor answers with the identifier its own local APIC
         * reports, not the one the firmware listed for the core number. A
         * mismatch means the MADT and the machine disagree: the core stops
         * on its stack instead of joining the run queues of other numbers.
         */
        seen = __atomic_load_n(&g_ap_bringup[cpu].seen_id, __ATOMIC_ACQUIRE);
        if (seen != g_ap_bringup[cpu].apic_id) {
            kinfo("x86-64: ap cpu %u apic 0x%x answered as 0x%x, not accepted", (unsigned)cpu,
                  (unsigned)g_ap_bringup[cpu].apic_id, (unsigned)seen);
            ap_phase(cpu, AP_PHASE_ACCEPTED, 0u, false);
            continue;
        }

        /* The core is in C with its own GDT, IDT, stack and clock. From here
         * the boot owns the record, and the release below lets it into the
         * kernel. */
        __atomic_store_n(&g_online[cpu], true, __ATOMIC_RELEASE);
        ap_phase(cpu, AP_PHASE_ACCEPTED, 0u, true);
        started++;
    }

    g_failures = (available > started) ? available - started : 0u;
    g_checked_in = started;
}

/*
 * The replay in the log of the kernel. The live lines have already been on
 * the framebuffer and COM1. A summary of every core is put in the ring of the
 * kernel, where `dmesg` can still find it after the boot; the full sequence
 * of a core that failed is repeated as well, because that is the part a
 * person needs when the machine came up with fewer cores than the firmware
 * named.
 */
void port_x86_64_smp_log_bringup(void)
{
    k_u32_t available = port_x86_64_cpu_available();
    k_u32_t limit = (available < KERNEL_CPUS_MAX) ? available : KERNEL_CPUS_MAX;
    k_u32_t cpu;
    k_u32_t i;

    kinfo("x86-64: ap trampoline from %s, signature %s",
          g_trampoline_source == 1u ? "loader"
                                    : (g_trampoline_source == 2u ? "kernel image" : "none"),
          g_trampoline_ok ? "ok" : "missing");

    for (cpu = 1u; cpu < limit; cpu++) {
        const struct ap_bringup *record = &g_ap_bringup[cpu];
        k_u32_t phase = record->phase < AP_PHASE_COUNT ? record->phase : AP_PHASE_NONE;

        kinfo("x86-64: ap cpu %u apic 0x%x: last phase %s %s after %u us, waited %u us",
              (unsigned)cpu, (unsigned)record->apic_id, g_phase_names[phase],
              record->ok ? "ok" : "failed", (unsigned)record->elapsed_us,
              (unsigned)record->waited_us);
    }

    for (i = 0u; i < g_ap_log_count; i++) {
        k_u32_t owner = g_ap_log_cpu[i];

        if (owner < KERNEL_CPUS_MAX && !g_ap_bringup[owner].ok) {
            kinfo("%s", g_ap_log[i]);
        }
    }

    kinfo("x86-64: %u AP(s) reached C of %u named by the firmware, %u failure(s)",
          (unsigned)g_checked_in - 1u, (unsigned)available - 1u, (unsigned)g_failures);
}

void port_x86_64_smp_release(void)
{
    __atomic_store_n(&g_released, true, __ATOMIC_RELEASE);
}

k_u32_t port_x86_64_smp_failures(void)
{
    return g_failures;
}

k_u32_t port_x86_64_smp_core_status(k_u32_t cpu)
{
    k_u32_t status = 0;

    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    if (__atomic_load_n(&g_reached[cpu], __ATOMIC_ACQUIRE)) {
        status |= PORT_X86_64_CORE_REACHED;
    }

    if (__atomic_load_n(&g_online[cpu], __ATOMIC_ACQUIRE)) {
        status |= PORT_X86_64_CORE_ONLINE;
    }

    if (__atomic_load_n(&g_apic_ok[cpu], __ATOMIC_ACQUIRE)) {
        status |= PORT_X86_64_CORE_APIC;
    }

    if (__atomic_load_n(&g_timer_ok[cpu], __ATOMIC_ACQUIRE)) {
        status |= PORT_X86_64_CORE_TIMER;
    }

    return status;
}

/*
 * The first C of an application processor. The trampoline of the port lands
 * here with the number of the core in RDI, the stack of the core in RSP, the
 * interrupts off and no way back.
 *
 * The identity of the core comes first and before any kernel call: every
 * call of the kernel may ask which core it is (the critical section of the
 * port does), and the answer is a word at the base of GS.
 */
void port_x86_64_ap_entry(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        /* No slot in the kernel for this number: it must not run the kernel
         * at all, and it says so by stopping where it is. */
        port_cli();

        for (;;) {
            port_hlt();
        }
    }

    port_x86_64_cpu_set_self(cpu);

    port_x86_64_gdt_init_cpu(cpu);

    /*
     * The identity is written a second time, and this is not a belt and
     * braces: loading a segment register in long mode replaces the hidden
     * base of the segment with the base of the descriptor, and gdt_load()
     * reloads GS with the null selector of the new table, which is a base of
     * zero. The first write is what makes gdt_init_cpu() build the table of
     * *this* core; this one is what survives the table it loaded.
     */
    port_x86_64_cpu_set_self(cpu);

    port_x86_64_interrupts_load();

    /* The fast door of a system call is a register of the core (src/syscall.c).
     * A core that opens it late takes the calls of its programs the slow way,
     * through the vector 0x80, which is still in the table of every core. */
    port_x86_64_syscall_init_cpu();

    /*
     * CR0.WP is a register of the core and the boot set it only on its own:
     * without it a copy of the kernel into a read-only page of a program does
     * not fault, the copy-on-write of the page never runs, and the program
     * keeps its text executable after a call wrote into it (the third fault
     * of /bin/nx, which answers differently on a core that skipped this).
     */
    port_x86_64_paging_init_cpu();

    /*
     * The core says which identifier its own local APIC reports before the
     * boot is told it reached C. The boot compares this with the identifier
     * of the MADT entry and refuses a core that arrived under another number:
     * it is the check that keeps a machine of many sockets from putting two
     * processors in one slot of the run queues.
     */
    __atomic_store_n(&g_ap_bringup[cpu].seen_id, port_x86_64_apic_id(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_reached[cpu], true, __ATOMIC_RELEASE);

    /*
     * The scheduler of the kernel does not exist yet when the port is
     * brought up, and its array has room for the cores the boot accepted
     * only. The core waits here with the interrupts off and touches nothing.
     */
    while (!__atomic_load_n(&g_released, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    if (!__atomic_load_n(&g_online[cpu], __ATOMIC_ACQUIRE)) {
        /* The boot did not accept this core. It never enters the scheduler:
         * a core outside the array of kernel/cpu.c would corrupt the queues
         * of the cores that are in it. */
        port_cli();

        for (;;) {
            port_hlt();
        }
    }

    /*
     * The local APIC of a core is enabled through its own spurious vector
     * register, and the boot enabled only its own. The call is the one of
     * the boot - the page is mapped already and shared - and it is a no-op
     * for the mapping on this core.
     */
    if (port_x86_64_apic_init()) {
        __atomic_store_n(&g_apic_ok[cpu], true, __ATOMIC_RELEASE);

        if (port_x86_64_apic_start_timer(1000u)) {
            __atomic_store_n(&g_timer_ok[cpu], true, __ATOMIC_RELEASE);
        }
    }

    /* The idle loop must know whether this core has a clock of its own: a
     * core without one polls its queue instead of halting on a wake-up that
     * will never come. */
    port_x86_64_ap_timer_ready(cpu, __atomic_load_n(&g_timer_ok[cpu], __ATOMIC_ACQUIRE));

    port_x86_64_ap_sched_init(cpu, g_ap_stack_top[cpu]);

    /* The interrupts of a context travel with it, and the idle context of a
     * core has them on: the timer of the core is its wake-up. */
    port_sti();

    port_x86_64_ap_idle();
}

/* --- the command of the shell ---------------------------------------------- */

static void smp_print_lock(void)
{
    if (g_print_lock != NULL) {
        (void)k_port_mutex_lock(g_print_lock);
    }
}

static void smp_print_unlock(void)
{
    if (g_print_lock != NULL) {
        (void)k_port_mutex_unlock(g_print_lock);
    }
}

/*
 * The state of the machine: the cores of the kernel, one line each, and the
 * check of the run queues. The lines are put together under the scheduler
 * lock and printed after it, because the console of the port may sleep on its
 * mutex and a lock held across a sleep is a deadlock waiting for the second
 * core.
 */
static void smp_report_wake(void);
static void smp_report_console(void);

/*
 * The state of the bring-up, including the cores that never joined the
 * scheduler. This is the line to read on a machine that boots on one core:
 * it names the phase the last AP reached and the time the boot waited.
 */
static void smp_report_bringup(void)
{
    k_u32_t available = port_x86_64_cpu_available();
    k_u32_t limit = (available < KERNEL_CPUS_MAX) ? available : KERNEL_CPUS_MAX;
    k_u32_t cpu;

    kshell_printf("smp: ap trampoline from %s, signature %s\n",
                  g_trampoline_source == 1u ? "loader"
                                            : (g_trampoline_source == 2u ? "kernel image" : "none"),
                  g_trampoline_ok ? "ok" : "missing");

    for (cpu = 1u; cpu < limit; cpu++) {
        const struct ap_bringup *record = &g_ap_bringup[cpu];
        k_u32_t phase = record->phase < AP_PHASE_COUNT ? record->phase : AP_PHASE_NONE;

        kshell_printf("smp: ap cpu %u apic 0x%x: %s %s, self 0x%x, %u us of %u us\n", (unsigned)cpu,
                      (unsigned)record->apic_id, g_phase_names[phase], record->ok ? "ok" : "failed",
                      (unsigned)record->seen_id, (unsigned)record->elapsed_us,
                      (unsigned)record->waited_us);
    }
}

static int smp_status(void)
{
    struct line {
        k_u32_t apic_id;
        k_u32_t ticks;
        char current[TASK_NAME_MAX];
    } lines[KERNEL_CPUS_MAX];
    k_u32_t count = k_cpu_count();
    k_u32_t cpu;
    k_size_t problems;

    if (count > KERNEL_CPUS_MAX) {
        count = KERNEL_CPUS_MAX;
    }

    memset(lines, 0, sizeof(lines));

    k_cpu_lock();

    for (cpu = 0; cpu < count; cpu++) {
        struct k_cpu *record = k_cpu_by_id(cpu);

        if (record == NULL) {
            continue;
        }

        lines[cpu].apic_id = port_x86_64_cpu_apic_id(cpu);
        lines[cpu].ticks = record->ticks;

        if (record->current != NULL) {
            memcpy(lines[cpu].current, record->current->name, TASK_NAME_MAX);
            lines[cpu].current[TASK_NAME_MAX - 1] = '\0';
        } else {
            memcpy(lines[cpu].current, "idle", sizeof("idle"));
        }
    }

    k_cpu_unlock();

    smp_print_lock();

    kshell_printf("smp: %u core(s) in use of %u reported, apic 0x%x\n", (unsigned)count,
                  (unsigned)port_x86_64_cpu_available(), (unsigned)port_x86_64_apic_base());

    for (cpu = 0; cpu < count; cpu++) {
        kshell_printf("smp: cpu %u apic 0x%x ticks %u current %s\n", (unsigned)cpu,
                      (unsigned)lines[cpu].apic_id, (unsigned)lines[cpu].ticks, lines[cpu].current);
    }

    smp_report_bringup();

    smp_print_unlock();

    problems = k_sched_validate();

    smp_print_lock();

    if (problems == 0u) {
        kshell_printf("smp: queues consistent\n");
    } else {
        kshell_printf("smp: %u problem(s) of the queues\n", (unsigned)problems);
    }

    kshell_printf("smp: the command ran on cpu %u\n", (unsigned)k_cpu_id());

    smp_print_unlock();

    smp_report_wake();
    smp_report_console();

    return KSHELL_OK;
}

/*
 * The wake of a core (ADR 0012, docs/debts.md K8): the tasks this core started
 * in the body of the interrupt of another one - that is, before its own timer
 * could have offered them - and the ones it started at its tick. The numbers
 * are the ones the smoke test of the two cores reads.
 */
static void smp_report_wake(void)
{
    k_u32_t count = k_cpu_count();
    k_u32_t wake = k_sched_remote_wakes();
    k_u32_t cpu;

    if (count > KERNEL_CPUS_MAX) {
        count = KERNEL_CPUS_MAX;
    }

    smp_print_lock();

    kshell_printf("smp: wake before the tick:");
    for (cpu = 0; cpu < count; cpu++) {
        kshell_printf(" cpu %u %u", (unsigned)cpu, (unsigned)port_x86_64_wake_before_tick(cpu));
    }
    kshell_printf(" (started by the ipi of the other core)\n");

    kshell_printf("smp: started at the tick:");
    for (cpu = 0; cpu < count; cpu++) {
        kshell_printf(" cpu %u %u", (unsigned)cpu, (unsigned)port_x86_64_tick_start(cpu));
    }
    kshell_printf(" (started by the timer of the core)\n");

    kshell_printf("smp: the kernel handed %u task(s) to another core\n", (unsigned)wake);

    {
        /* The measurement of the last hand-over: the tick of the core when the
         * task was put in its queue, and the tick of the core when it began to
         * run it. The same tick means the task started before the timer of that
         * core could have offered it, which is what the wake is for. */
        k_u32_t last_cpu = 0u;
        k_u32_t last_tick = 0u;

        k_sched_last_remote_wake(&last_cpu, &last_tick);

        if (last_cpu < count) {
            kshell_printf("smp: the last hand-over went to cpu %u at tick %u, the core started it "
                          "at tick %u\n",
                          (unsigned)last_cpu, (unsigned)last_tick,
                          (unsigned)port_x86_64_wake_tick(last_cpu));
        }
    }

    for (cpu = 0; cpu < count; cpu++) {
        kshell_printf("smp: cpu %u was woken %u time(s), %u ipi(s) taken, clock %s\n",
                      (unsigned)cpu, (unsigned)port_x86_64_wake_sent_cpu(cpu),
                      (unsigned)port_x86_64_ipi_received_cpu(cpu),
                      port_x86_64_timer_off(cpu) ? "stopped" : "running");
    }

    smp_print_unlock();
}

/* The console is part of the SMP story: a blocked serial reader must show up
 * as dropped bytes with a live RIP, not as a shell that silently stops. */
static void smp_report_console(void)
{
    struct port_x86_64_com1_stats stats;

    port_x86_64_com1_get_stats(&stats);

    smp_print_lock();

    kshell_printf("smp: console: THR waits %llu, timeouts %llu, dropped %llu\n",
                  (unsigned long long)stats.thr_waits, (unsigned long long)stats.thr_timeouts,
                  (unsigned long long)stats.dropped);

    if (stats.thr_timeouts != 0u) {
        kshell_printf("smp: console last: cpu %u pid %u rip 0x%08x%08x rsp 0x%08x%08x\n",
                      (unsigned)stats.last_cpu, (unsigned)stats.last_pid,
                      (unsigned)(stats.last_rip >> 32), (unsigned)(stats.last_rip & 0xFFFFFFFFu),
                      (unsigned)(stats.last_rsp >> 32), (unsigned)(stats.last_rsp & 0xFFFFFFFFu));
        kshell_printf("smp: console stack:");

        for (k_size_t i = 0; i < PORT_X86_64_COM1_STACK_WORDS; i++) {
            kshell_printf(" 0x%08x%08x", (unsigned)(stats.last_stack[i] >> 32),
                          (unsigned)(stats.last_stack[i] & 0xFFFFFFFFu));
        }

        kshell_printf("\n");
    }

    smp_print_unlock();
}

/* One worker: it says which core ran it and ends, so that the line is the
 * proof the scheduler put a task of this core on it and not a figure of the
 * command that started it. */
static void smp_worker(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    smp_print_lock();
    kshell_printf("smp: worker %u on cpu %u\n", self != NULL ? (unsigned)self->pid : 0u,
                  (unsigned)k_cpu_id());
    smp_print_unlock();
}

#define SMP_WORKERS_MAX 8u
#define SMP_WORKER_TIMEOUT_MS 5000u

static bool smp_parse_count(const char *text, k_u32_t *out)
{
    k_u32_t value = 0;
    k_size_t i;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        value = value * 10u + (k_u32_t)(text[i] - '0');

        if (value > SMP_WORKERS_MAX) {
            return false;
        }
    }

    *out = value;
    return true;
}

/*
 * `smp workers <n>`: n tasks of the default priority, each one naming the
 * core it runs on. The scheduler gives a new task the next core in its round
 * robin, so a run of at least as many workers as cores puts one on each of
 * them; the command waits for every one of them before it says so.
 */
static int smp_workers(const char *text)
{
    struct task *made[SMP_WORKERS_MAX];
    k_u32_t count;
    k_u32_t created = 0;
    k_u32_t finished = 0;
    k_u32_t i;

    if (!smp_parse_count(text, &count) || count == 0u) {
        smp_print_lock();
        kshell_printf("smp: workers needs a count of 1 to %u\n", (unsigned)SMP_WORKERS_MAX);
        smp_print_unlock();
        return KSHELL_USAGE;
    }

    for (i = 0; i < count; i++) {
        struct task *t = task_create("smpworker", smp_worker, NULL, TASK_PRIO_DEFAULT);

        if (t == NULL) {
            break;
        }

        made[created] = t;
        created++;
    }

    for (i = 0; i < created; i++) {
        int code = 0;

        if (task_wait(made[i]->pid, &code, SMP_WORKER_TIMEOUT_MS) == 0) {
            finished++;
        }
    }

    smp_print_lock();

    if (finished == created) {
        kshell_printf("smp: %u worker(s) done\n", (unsigned)created);
    } else {
        kshell_printf("smp: %u worker(s) did not finish of %u\n", (unsigned)(created - finished),
                      (unsigned)created);
    }

    smp_print_unlock();

    return finished == created ? KSHELL_OK : KSHELL_ERROR;
}

/*
 * `smp ping`: an IPI to the next core, and the count of the interrupts that
 * core has taken. The two numbers are the proof of the path: a vector that is
 * in the table of the port and a core that runs its body, not a comment about
 * one.
 */
static int smp_ping(void)
{
    k_u32_t self = k_cpu_id();
    k_u32_t other;
    k_u32_t before;
    k_u32_t after;

    if (k_cpu_count() < 2u) {
        smp_print_lock();
        kshell_printf("smp: ping needs a second core, and the kernel runs on one\n");
        smp_print_unlock();
        return KSHELL_ERROR;
    }

    other = (self + 1u) % k_cpu_count();

    before = port_x86_64_ipi_received_cpu(other);
    port_x86_64_apic_send_ipi(port_x86_64_cpu_apic_id(other), PORT_X86_64_VECTOR_APIC_IPI);
    tsc_delay(10000u);
    after = port_x86_64_ipi_received_cpu(other);

    smp_print_lock();
    kshell_printf("smp: ping cpu %u: ipi count %u -> %u\n", (unsigned)other, (unsigned)before,
                  (unsigned)after);
    kshell_printf("%s\n", after > before ? "smp: ping ok" : "smp: ping failed");
    smp_print_unlock();

    return after > before ? KSHELL_OK : KSHELL_ERROR;
}

/* --- the lines of the machine (ADR 0012) --- */

/*
 * `smp irq`: where the two lines of the port are delivered. The I/O APIC of
 * the firmware has an entry per line and names a core in it; the 8259 has one
 * output and it reaches the core of the boot. The counters say which core
 * took the interrupt, so a run that moves a line can see it arrive where it
 * was sent - a keyboard of a machine of two cores is the test of the table.
 */
static int smp_irq_report(void)
{
    k_u32_t count = k_cpu_count();
    k_u32_t cpu;
    k_u32_t line;

    if (count > KERNEL_CPUS_MAX) {
        count = KERNEL_CPUS_MAX;
    }

    smp_print_lock();

    if (port_x86_64_irq_on_ioapic()) {
        kshell_printf("smp: the I/O APIC at 0x%x, %u line(s), the 8259 is masked\n",
                      (unsigned)port_x86_64_ioapic_phys(), (unsigned)port_x86_64_ioapic_entries());

        for (line = 0; line < 2u; line++) {
            kshell_printf("smp: line %s: isa irq %u -> gsi %u, vector 0x%x, cpu %u\n",
                          line == PORT_X86_64_IRQ_TIMER ? "timer" : "keyboard", (unsigned)line,
                          (unsigned)port_x86_64_ioapic_line_gsi(line),
                          (unsigned)port_x86_64_ioapic_line_vector(line),
                          (unsigned)port_x86_64_ioapic_line_cpu(line));
        }
    } else {
        kshell_printf("smp: no I/O APIC, the 8259 delivers both lines to the cpu 0\n");
    }

    for (line = 0; line < 2u; line++) {
        kshell_printf("smp: irq 0x%x (%s):", 0x20u + (unsigned)line,
                      line == PORT_X86_64_IRQ_TIMER ? "timer" : "keyboard");

        for (cpu = 0; cpu < count; cpu++) {
            kshell_printf(" cpu %u %u", (unsigned)cpu, (unsigned)port_x86_64_irq_count(line, cpu));
        }

        kshell_printf("\n");
    }

    smp_print_unlock();

    return KSHELL_OK;
}

/*
 * `smp irq keyboard <cpu>`: point the keyboard line at another core of the
 * I/O APIC. The line keeps its vector and its polarity; only the destination
 * of the entry changes, and the next key is taken by the core that is named.
 * A machine whose keyboard is on the 8259 has no such table, and the command
 * says so instead of pretending to move it.
 */
static int smp_irq_keyboard(const char *text)
{
    k_u32_t cpu;
    k_u32_t vector;

    if (!port_x86_64_irq_on_ioapic()) {
        smp_print_lock();
        kshell_printf("smp: the keyboard is on the 8259, there is no I/O APIC to move it\n");
        smp_print_unlock();
        return KSHELL_ERROR;
    }

    if (!smp_parse_count(text, &cpu) || cpu >= k_cpu_count()) {
        smp_print_lock();
        kshell_printf("smp: the keyboard needs a core of 0 to %u\n",
                      (unsigned)(k_cpu_count() - 1u));
        smp_print_unlock();
        return KSHELL_USAGE;
    }

    vector = port_x86_64_ioapic_line_vector(PORT_X86_64_IRQ_KEYBOARD);

    if (!port_x86_64_ioapic_route(PORT_X86_64_IRQ_KEYBOARD, vector, cpu)) {
        smp_print_lock();
        kshell_printf("smp: the I/O APIC did not take the line of the keyboard\n");
        smp_print_unlock();
        return KSHELL_ERROR;
    }

    smp_print_lock();
    kshell_printf("smp: the keyboard line is now on cpu %u (gsi %u, vector 0x%x)\n", (unsigned)cpu,
                  (unsigned)port_x86_64_ioapic_line_gsi(PORT_X86_64_IRQ_KEYBOARD),
                  (unsigned)vector);
    smp_print_unlock();

    return KSHELL_OK;
}

static int smp_irq(int argc, char **argv)
{
    if (argc == 0) {
        return smp_irq_report();
    }

    if (argc >= 2 && strcmp(argv[0], "keyboard") == 0) {
        return smp_irq_keyboard(argv[1]);
    }

    smp_print_lock();
    kshell_printf("smp: usage: smp irq [keyboard <cpu>]\n");
    smp_print_unlock();

    return KSHELL_USAGE;
}

/* --- the wake of a core (ADR 0012, docs/debts.md K8) --- */

/*
 * The experiment of the wake, and the reason it is an experiment and not a
 * claim: the clock of the other core is stopped for its length.
 *
 * A core with a timer of its own would pick a task from its queue at its next
 * tick whether anybody told it or not, and a run could not tell the two paths
 * apart. With the timer stopped, the only way a task begins on that core is
 * the interrupt of this one, and the counters of the port say how many did.
 * The clock is started again before the command returns, whatever the tasks
 * did: a core left without a clock is a machine that stops taking work.
 */
#define SMP_WAKE_TASKS 32u
#define SMP_WAKE_SLEEP_MS 120u
#define SMP_WAKE_WAIT_MS 20u
#define SMP_WAKE_TIMEOUT_MS 5000u
#define SMP_WAKE_STOP_TIMEOUT_MS 200u

struct smp_wake_state {
    k_u32_t cpu;   /* the core that ran the task after its sleep */
    k_u32_t ticks; /* the ticks of that core at that moment */
    bool done;
};

/* One task of the experiment: it waits on the core it was given and comes
 * back to say which core and which tick of it ran it. */
static void smp_wake_worker(void *arg)
{
    struct smp_wake_state *state = arg;

    k_sched_sleep_ms(SMP_WAKE_SLEEP_MS);

    state->cpu = k_cpu_id();
    state->ticks = k_sched_cpu_ticks(k_cpu_id());
    state->done = true;
}

static int smp_wake(void)
{
    struct smp_wake_state states[SMP_WAKE_TASKS];
    struct task *made[SMP_WAKE_TASKS];
    k_u32_t self = k_cpu_id();
    k_u32_t target;
    k_u32_t before_wake;
    k_u32_t before_ipi;
    k_u32_t after_wake;
    k_u32_t after_ipi;
    k_u32_t before_ticks;
    k_u32_t after_ticks;
    k_u32_t on_target = 0;
    k_u32_t created = 0;
    k_u32_t finished = 0;
    k_u32_t i;

    if (k_cpu_count() < 2u) {
        smp_print_lock();
        kshell_printf("smp: wake needs a second core, and the kernel runs on one\n");
        smp_print_unlock();
        return KSHELL_ERROR;
    }

    target = (self + 1u) % k_cpu_count();

    /*
     * The clock of the core 0 is the clock of the kernel: the millisecond of
     * the machine is the tick of that core, and the deadline of a task is a
     * number of that clock. Stopping it would stop the deadline of the very
     * tasks of the experiment, so the command only ever stops another core
     * and says so when it cannot.
     */
    if (target == 0u) {
        smp_print_lock();
        kshell_printf("smp: wake: the clock of the core 0 is the clock of the kernel, "
                      "run the command on the core 0\n");
        smp_print_unlock();
        return KSHELL_ERROR;
    }

    memset(states, 0, sizeof(states));

    /* The clock of the other core goes off, and the command waits until the
     * core says it did: a task created before that would be picked by the
     * timer it is meant to be independent of. */
    port_x86_64_timer_off_request(target);

    for (i = 0; i < SMP_WAKE_STOP_TIMEOUT_MS; i++) {
        if (port_x86_64_timer_off(target)) {
            break;
        }

        k_sched_sleep_ms(1u);
    }

    if (!port_x86_64_timer_off(target)) {
        smp_print_lock();
        kshell_printf("smp: the core %u did not stop its clock, the wake is not tested\n",
                      (unsigned)target);
        smp_print_unlock();
        port_x86_64_timer_on_request(target);
        return KSHELL_ERROR;
    }

    before_ticks = k_sched_cpu_ticks(target);
    before_wake = k_sched_remote_wakes();
    before_ipi = port_x86_64_wake_before_tick(target);

    for (i = 0; i < SMP_WAKE_TASKS; i++) {
        struct task *t = task_create("smpwake", smp_wake_worker, &states[i], TASK_PRIO_DEFAULT);

        if (t == NULL) {
            break;
        }

        made[created] = t;

        if (k_sched_task_cpu(t) == target) {
            on_target++;
        }

        created++;
    }

    /* The tasks of the experiment are on the machine now; the ones on the
     * other core are waiting for their sleep. */
    k_sched_sleep_ms(SMP_WAKE_WAIT_MS);

    after_wake = k_sched_remote_wakes();
    after_ipi = port_x86_64_wake_before_tick(target);

    for (i = 0; i < created; i++) {
        int code = 0;

        if (task_wait(made[i]->pid, &code, SMP_WAKE_TIMEOUT_MS) == 0) {
            finished++;
        }
    }

    after_ticks = k_sched_cpu_ticks(target);

    /* The clock of the core comes back before anything else is said: the
     * machine is not left with a core that only an interrupt can wake. */
    port_x86_64_timer_on_request(target);

    for (i = 0; i < SMP_WAKE_STOP_TIMEOUT_MS; i++) {
        if (!port_x86_64_timer_off(target)) {
            break;
        }

        k_sched_sleep_ms(1u);
    }

    smp_print_lock();

    kshell_printf("smp: wake: %u task(s) of %u started, %u of them on cpu %u\n", (unsigned)created,
                  (unsigned)SMP_WAKE_TASKS, (unsigned)on_target, (unsigned)target);
    kshell_printf("smp: wake: the kernel handed %u -> %u task(s) to another core\n",
                  (unsigned)before_wake, (unsigned)after_wake);
    kshell_printf("smp: wake: cpu %u started %u -> %u task(s) before its tick\n", (unsigned)target,
                  (unsigned)before_ipi, (unsigned)after_ipi);
    kshell_printf("smp: wake: the ticks of cpu %u went %u -> %u with its clock stopped\n",
                  (unsigned)target, (unsigned)before_ticks, (unsigned)after_ticks);

    for (i = 0; i < created; i++) {
        kshell_printf("smp: wake: the task %u ran on cpu %u at tick %u\n", (unsigned)(i + 1u),
                      (unsigned)states[i].cpu, (unsigned)states[i].ticks);
    }

    if (finished == created) {
        kshell_printf("smp: wake: %u task(s) done\n", (unsigned)created);
    } else {
        kshell_printf("smp: wake: %u task(s) did not finish of %u\n",
                      (unsigned)(created - finished), (unsigned)created);
    }

    kshell_printf("smp: wake: the clock of cpu %u is %s\n", (unsigned)target,
                  port_x86_64_timer_off(target) ? "still stopped" : "running again");

    if (after_ipi > before_ipi && after_ticks == before_ticks) {
        kshell_printf("smp: wake ok: the tasks of cpu %u started by the ipi, before its tick\n",
                      (unsigned)target);
    } else {
        kshell_printf("smp: wake failed: cpu %u did not start a task by the ipi\n",
                      (unsigned)target);
    }

    smp_print_unlock();

    return (finished == created && after_ipi > before_ipi) ? KSHELL_OK : KSHELL_ERROR;
}

static int cmd_smp(int argc, char **argv)
{
    if (argc < 2) {
        return smp_status();
    }

    if (strcmp(argv[1], "workers") == 0) {
        return smp_workers(argc > 2 ? argv[2] : NULL);
    }

    if (strcmp(argv[1], "ping") == 0) {
        return smp_ping();
    }

    if (strcmp(argv[1], "irq") == 0) {
        return smp_irq(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "wake") == 0) {
        return smp_wake();
    }

    smp_print_lock();
    kshell_printf("smp: usage: smp [workers <n>|ping|irq [keyboard <cpu>]|wake]\n");
    smp_print_unlock();

    return KSHELL_USAGE;
}

int port_x86_64_smp_register(void)
{
    int rc;

    /* The mutex of the lines is made here, before the shell exists and on one
     * core, so that two cores never create it at once. */
    g_print_lock = k_port_mutex_create();

    rc = kshell_register("smp", "the cores of the machine: smp [workers <n>|ping|irq|wake]",
                         cmd_smp);

    return rc;
}

#else /* !KERNEL_SMP */

void port_x86_64_smp_init(void)
{
    /*
     * The one-core kernel of 1.0: the flag is the promise, not a property of
     * the machine, and a core the kernel may not use is a core not started.
     */
}

void port_x86_64_smp_release(void) {}

/* A one-core build sends no IPI, so there is no trampoline to install and no
 * bring-up to replay. The kernel still calls both steps of the boot. */
void port_x86_64_ap_trampoline_install(void) {}
void port_x86_64_smp_log_bringup(void) {}

k_u32_t port_x86_64_smp_failures(void)
{
    return 0u;
}

k_u32_t port_x86_64_smp_core_status(k_u32_t cpu)
{
    K_UNUSED(cpu);

    return PORT_X86_64_CORE_REACHED | PORT_X86_64_CORE_ONLINE;
}

#endif /* KERNEL_SMP */

k_u32_t port_x86_64_smp_checked_in(void)
{
    return __atomic_load_n(&g_checked_in, __ATOMIC_RELAXED);
}

bool port_x86_64_smp_apic_ready(void)
{
    return port_x86_64_apic_present();
}
