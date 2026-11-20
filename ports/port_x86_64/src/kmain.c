/*
 * Micro OS — the entry point on a QEMU x86-64 machine.
 *
 * The loader hands the kernel over in long mode with a stack and a map of
 * the memory; from here on the boot is the same as on the board: the kernel
 * checks the contract of the port, prints its banner, initialises the
 * subsystems it was given and leaves the machine to the init process.
 *
 * The init process starts the shell and then idles. Until the task substrate
 * of the port exists there is no init process at all, and the machine says
 * so and keeps breathing instead of stopping without a word: a kernel that
 * stays alive is the difference between "the tasks are not done yet" and
 * "the boot is broken".
 *
 * Between the banner and the init process the boot runs a program of its own
 * (see program_demo below): this is the machine of the project that has the
 * memory to place an image and the processor to jump into it, so a program of
 * the phase is a program that really runs and not a figure of speech.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/board.h"
#include "kernel/boot.h"
#include "kernel/exec.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"

#include "kshell/kshell.h"

/* The body of the shell task, in src/shell.c. */
void port_x86_64_shell_run(void);

/* The nap of the tasks of the port that have nothing to do right now. */
#define PORT_IDLE_MS 1000u

static const struct kernel_board g_board = {
    .name = "qemu_x86_64",
    .desc = "QEMU x86-64 machine",
    .soc = "QEMU virtio",
    .flash_mb = 0,
    .psram_kb = 0,
    .lcd = "none (the console is COM1)",
    .imu = "none",
    .rtc = "the clock of the port",
    .audio = "none",
    .power = "none",
    .buttons = "none",
    .has_buzzer = false,
    .has_ir = false,
    .has_mic = false,
};

static void shell_task(void *arg)
{
    K_UNUSED(arg);

    port_x86_64_shell_run();
}

/* --- the program of the milestone ---------------------------------------- */

/*
 * A program, built in C out of the bytes of the machine code, instead of
 * being read from a file: the boot of this port has no filesystem yet, and
 * the point of the milestone is that an image of the MOX1 format really runs.
 * The image is the one tools/mkprog.sh makes out of an object, so the parser
 * of the kernel and the jump of the port are exercised by the format itself
 * and not by a shortcut.
 *
 * The listing of the bytes, which an assembler would turn into the same
 * numbers:
 *
 *     lea  rsi, [rip + message]   the string, in the data of the image
 *     mov  edx, 0x3f8             the data register of COM1
 * loop:
 *     mov  al, [rsi]
 *     test al, al
 *     jz   done
 *     out  dx, al                 one byte at a time, no libc and no kernel
 *     inc  rsi
 *     jmp  loop
 * done:
 *     ret                         the exit the port left on the stack
 * message:
 *     "PROGRAM: hello from 0x02000000\r\n" and a zero
 *
 * The string sits in the data segment and not in the text on purpose: the
 * port copies the text and the data, clears the bss and then jumps, and a
 * message that came out of the data is the proof that the copy of the data
 * happened (a port that copied the text alone would print zeros here).
 *
 * The last instruction is a ret and not the halt of a program that owns the
 * machine: this demonstration runs at the boot, before the shell exists, and
 * the shell of the smoke test still has to come up. The port leaves the
 * address of its exit on the stack of the program for exactly that reason
 * (see port_exec.c); a program that halts never gets there.
 */
#define PROG_CODE_SIZE 25u
#define PROG_TEXT_SIZE 28u /* the code, rounded up to the four bytes of the format */
#define PROG_DATA_SIZE 36u /* the message, rounded up the same way */

static const char g_prog_message[] = "PROGRAM: hello from 0x02000000\r\n";

static const k_u8_t g_prog_code[PROG_CODE_SIZE] = {
    0x48, 0x8d, 0x35, 0x00, 0x00, 0x00, 0x00, /* lea rsi, [rip + message] */
    0xba, 0xf8, 0x03, 0x00, 0x00,             /* mov edx, 0x03f8 */
    0x8a, 0x06,                               /* mov al, [rsi] */
    0x84, 0xc0,                               /* test al, al */
    0x74, 0x06,                               /* jz done (the ret) */
    0xee,                                     /* out dx, al */
    0x48, 0xff, 0xc6,                         /* inc rsi */
    0xeb, 0xf4,                               /* jmp loop */
    0xc3,                                     /* ret */
};

/*
 * The header of the format wants sizes that are multiples of four, and the
 * two segments have to hold what the program is made of; the padding of a
 * segment is part of the image and is ignored, as the format allows.
 */
_Static_assert(PROG_CODE_SIZE <= PROG_TEXT_SIZE, "the code does not fit the text");
_Static_assert(sizeof(g_prog_message) <= PROG_DATA_SIZE, "the message does not fit the data");
_Static_assert((PROG_TEXT_SIZE & 3u) == 0, "the text is not a multiple of four");
_Static_assert((PROG_DATA_SIZE & 3u) == 0, "the data is not a multiple of four");

static k_u8_t g_program_image[K_EXEC_HEADER_SIZE + PROG_TEXT_SIZE + PROG_DATA_SIZE];

static void put_u32(k_u8_t *where, k_u32_t value)
{
    where[0] = (k_u8_t)(value & 0xFFu);
    where[1] = (k_u8_t)((value >> 8) & 0xFFu);
    where[2] = (k_u8_t)((value >> 16) & 0xFFu);
    where[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* The image in memory: the header of kernel/exec.h, the text, the data. */
static k_size_t program_image_build(k_u8_t *image)
{
    /*
     * The displacement of a rip-relative lea is measured from the end of the
     * instruction, which is where the RIP points when it runs. The lea ends
     * seven bytes into the text and the message is the first byte of the
     * data, so the distance is the text minus the lea.
     */
    k_u32_t displacement = PROG_TEXT_SIZE - 7u;
    k_u8_t *text = image + K_EXEC_HEADER_SIZE;
    k_u8_t *data = text + PROG_TEXT_SIZE;

    memset(image, 0, K_EXEC_HEADER_SIZE + PROG_TEXT_SIZE + PROG_DATA_SIZE);
    memcpy(text, g_prog_code, PROG_CODE_SIZE);
    put_u32(text + 3u, displacement);
    memcpy(data, g_prog_message, sizeof(g_prog_message));

    put_u32(image + 0u, K_EXEC_MAGIC);
    put_u32(image + 4u, K_EXEC_VERSION);
    put_u32(image + 8u, 0u);                  /* flags */
    put_u32(image + 12u, K_EXEC_HEADER_SIZE); /* entry: the first byte of the text */
    put_u32(image + 16u, PROG_TEXT_SIZE);
    put_u32(image + 20u, PROG_DATA_SIZE);
    put_u32(image + 24u, 0u); /* bss */
    put_u32(image + 28u, 0u); /* reserved */

    return K_EXEC_HEADER_SIZE + PROG_TEXT_SIZE + PROG_DATA_SIZE;
}

/*
 * Run the program of the milestone at the boot. The parse and the run are the
 * two calls the system call layer makes, so what runs here is the whole path
 * of a program and not a copy of it.
 */
static void program_demo(void)
{
    struct k_exec_image image;
    k_size_t size = program_image_build(g_program_image);

    if (k_exec_parse(g_program_image, size, &image) != 0) {
        kerror("x86-64: the image of the demonstration program is not a program");
        return;
    }

    kinfo("x86-64: a program runs");

    /*
     * The call comes back only because this program ends with a ret and the
     * port owns the exit; the kernel answers every return with an error,
     * which says nothing about this program. A program of the system never
     * returns, and the call then never comes back at all.
     */
    (void)k_exec_run(&image);
}

/*
 * The init process of the port: PID 1. It registers the commands of the
 * shell before the shell task exists, so that the first line a person types
 * already finds them, and then stays alive as the parent of the shell the
 * way init does on Linux.
 */
static void init_task(void *arg)
{
    K_UNUSED(arg);

    if (kshell_init() != KSHELL_OK) {
        kwarn("x86-64: not every command of the shell is registered");
    }

    if (task_create("shell", shell_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        kerror("x86-64: cannot start the shell");
    }

    for (;;) {
        k_sched_sleep_ms(PORT_IDLE_MS);
    }
}

void kmain(k_u64_t multiboot_info)
{
    struct kernel_boot_config cfg;

    port_x86_64_init(multiboot_info);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = g_board.name;
    cfg.board_desc = g_board.desc;
    cfg.board_info = &g_board;

    kernel_boot(&cfg);

    kinfo("x86-64: the kernel is up, heap %u KB free of %u KB",
          (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024));

    /*
     * The program of the milestone, after the boot and before the tasks: at
     * that point the machine is the boot context of the port and nothing
     * depends on the scheduler yet. That is the moment a program can be
     * entered without a task under it.
     */
    program_demo();

    if (task_create("init", init_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        /*
         * The context switch and the timer are the next milestone of the
         * port. Saying it once and sleeping is honest, and it keeps the
         * console of the machine alive for whoever looks at it.
         */
        kinfo("x86-64: no tasks yet");

        for (;;) {
            k_sched_sleep_ms(PORT_IDLE_MS);
        }
    }

    /* The init process owns the rest of the boot: the shell comes from it. */
    for (;;) {
        k_sched_sleep_ms(PORT_IDLE_MS);
    }
}
