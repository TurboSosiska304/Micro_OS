/*
 * Micro OS — the ESP32 port: running a program (ADR 0018).
 *
 * The board has no MMU and no way to make a page executable: the machine has
 * two kinds of memory and the loader uses both.
 *
 * The text lives in internal instruction RAM. The first 128 KiB of it are the
 * segment the linker fills (0x40080000..0x400A0000), and the 8 KiB below the
 * top are kept empty for the programs by an assertion of the linker script
 * (exec_windows.ld): the kernel refuses to link when its own code grows into
 * them. RAM is executable and does not go through the cache, so a copy is
 * enough — no mapping, no cache flush, and no flash is written.
 *
 * The data, the bss and the vectors of a program live in DRAM. This is not a
 * preference: the instruction RAM of the ESP32 is only 32-bit accessible for
 * data (the Kconfig option that emulates the other widths costs an exception
 * per byte), and a C program reads its strings byte by byte. The window is
 * the top of the static DRAM segment, which the build fixes with
 * CONFIG_ESP32_USE_FIXED_STATIC_RAM_SIZE: the heap starts at the end of it, so
 * the arena is outside both the static data and the heap. The same assertion
 * of the linker script keeps the static data out of the window.
 *
 * A program is a task of the scheduler, as on the host: `execve` makes it with
 * task_fork_shared(), waits for it and answers when it ends (ADR 0016). The
 * task has an 8 KiB stack of the port, and the program uses it as its own.
 *
 * There is exactly one program at a time: the windows are fixed addresses and
 * a program that asks for another one would overwrite the code it is running
 * from. The call that would do it answers ENOMEM (the kernel turns a refusal
 * of the port into that), and the rule is written in the ADR and in the
 * debts. The shell is a task of the kernel (its code is in flash), so the
 * ordinary path - the shell runs a program and waits - fits.
 *
 * The door is a function of the port, `port_mox_syscall()`, with the shape of
 * ksys_call(), and the program gets its address as the first argument of
 * _start. The `syscall` instruction is not available: the vector of ESP-IDF
 * takes the exception before the port sees it (ADR 0018 §3a).
 */
#include <string.h>

#include "heap_memory_layout.h"

#include "kernel/console.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#include "port_esp32.h"

/*
 * The windows of the programs. The numbers are half of a contract with
 * components/port_esp32/programs/program_esp32.ld (the linker script of a
 * program) and with exec_windows.ld (the assertion that keeps them free); the
 * text is below the top of IRAM, the data at the top of the static DRAM
 * segment.
 */
#define PORT_EXEC_TEXT_BASE ((k_size_t)0x4009E000u)
#define PORT_EXEC_TEXT_SIZE (8u * 1024u)
#define PORT_EXEC_DATA_BASE ((k_size_t)0x3FFCC000u)
#define PORT_EXEC_DATA_SIZE (8u * 1024u)

/*
 * The heap of the board must never hand out the text window: the free IRAM is
 * a pool of the heap (an allocation with MALLOC_CAP_EXEC prefers it), and a
 * task that received the bytes a program is copied to would be given the code
 * of another task. The macro places the range in a section that the heap
 * reads at startup, before app_main and before the first allocation; it is the
 * same mechanism the linker script of ESP-IDF uses for the static segments.
 *
 * The data window needs no reservation here: it is inside the static DRAM
 * segment, which the heap already excludes as a whole, and the build fixes
 * the end of that segment at 0x3FFCE000 (see exec_windows.ld). A second
 * reservation over the static range is an overlap the heap refuses, so the
 * fixed size is what protects the data and this line protects the text.
 */
SOC_RESERVE_MEMORY_REGION((intptr_t)PORT_EXEC_TEXT_BASE,
                          (intptr_t)(PORT_EXEC_TEXT_BASE + PORT_EXEC_TEXT_SIZE), mox_text);

/* A program of this system has no use for more arguments than a shell line. */
#define PORT_EXEC_ARGC_MAX 64u
#define PORT_EXEC_ARG_BYTES_MAX 1024u

/* The environment is bounded the same way. A word is a "NAME=VALUE" string. */
#define PORT_EXEC_ENVP_MAX 8u
#define PORT_EXEC_ENV_BYTES_MAX 512u

/* How long the caller of execve waits for the program it started. */
#define PORT_EXEC_WAIT_MS 10000u
#define PORT_EXEC_STEP_MS 5u

/* The name of the task a program runs in: the table keeps sixteen bytes. */
#define PORT_EXEC_TASK_NAME "program"

/* The shape of the door, and of the entry of a program of the board. */
typedef long (*port_exec_door_fn)(long number, long a0, long a1, long a2, long a3, long a4,
                                  long a5);
typedef void (*port_exec_entry_fn)(port_exec_door_fn door, long argc, char *const argv[],
                                   char *const envp[]);

/*
 * The state of a program between the call of execve and the first instruction
 * of its text. It lives in the heap of the kernel because the task starts as
 * soon as it is made; the body of the task copies what it needs and gives the
 * block back before it calls the program.
 */
struct port_exec_start {
    k_size_t entry;
    k_size_t argc;
    k_size_t argv;
    k_size_t envp;
};

/* The task that is a program right now, and whether its image owns the
 * windows. Both are written by the program task or by its parent while the
 * other is asleep; one program at a time makes that enough. */
static struct task *g_program;
static bool g_window_busy;

static k_size_t align_up(k_size_t value, k_size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/*
 * The door: the one function of the kernel a program may call. The numbers
 * and the arguments are those of ksys_call() (kernel/ksys.h), so a wrapper of
 * components/libc/user is the same call on both machines. The address of this
 * function is what _start() receives and keeps.
 *
 * It is not inlined into a copy per call site and it is not static: the
 * program holds its address, and the compiler must leave the function there.
 */
__attribute__((noinline)) long port_mox_syscall(long number, long a0, long a1, long a2, long a3,
                                                long a4, long a5)
{
    return (long)ksys_call((k_u32_t)number, (ksys_arg_t)a0, (ksys_arg_t)a1, (ksys_arg_t)a2,
                           (ksys_arg_t)a3, (ksys_arg_t)a4, (ksys_arg_t)a5);
}

/*
 * The context switch of the machine is the instruction sync of a program that
 * was just written: IRAM is not cached, but the pipeline of the core still has
 * to see the new bytes before it fetches them.
 */
static void instruction_sync(void)
{
    __asm__ volatile("isync" ::: "memory");
}

/*
 * The first code of the program. The task was made by k_port_exec_enter() as
 * an ordinary task of the scheduler; here it takes the role of the program and
 * calls its entry with the door, the count, the vector of the arguments and
 * the vector of the environment - the four registers of the windowed ABI.
 */
static void port_exec_program_task(void *arg)
{
    struct port_exec_start *start = (struct port_exec_start *)arg;
    port_exec_entry_fn entry;
    k_size_t argc;
    k_size_t argv;
    k_size_t envp;

    if (start == NULL) {
        task_exit(255);
        return;
    }

    entry = (port_exec_entry_fn)start->entry;
    argc = start->argc;
    argv = start->argv;
    envp = start->envp;
    k_port_free(start);

    /* From this point the door knows that the caller is a program: `exit`
     * ends this task and returns to the wait of execve. */
    g_program = task_self();

    entry(port_mox_syscall, (long)argc, (char *const *)(uintptr_t)argv,
          (char *const *)(uintptr_t)envp);

    /* A program ends through exit(); a return here is a broken image, and it
     * is taken down like one instead of running down the stack. */
    kerror("esp32: the program returned to the loader");
    task_exit(255);
}

bool k_port_exec_supported(void)
{
    return true;
}

int k_port_exec_arch(void)
{
    /* The bytes of this machine and the pull of the windowed call: Xtensa. */
    return (int)K_EXEC_ARCH_XTENSA;
}

void *k_port_exec_base(void)
{
    /* The window of the text. The header of the image is not copied there;
     * the kernel only uses the address to keep its sums inside the machine. */
    return (void *)(uintptr_t)PORT_EXEC_TEXT_BASE;
}

bool k_port_exec_active(void)
{
    /* A program is a task, and `exit` of that task is the end of the program;
     * a task of the kernel that is not the program keeps the old path. */
    return task_self() != NULL && task_self() == g_program;
}

void k_port_exec_exit(int code)
{
    /*
     * The task is going away, so it is no longer the program for the door.
     * The windows stay claimed: the parent that waits for this task releases
     * them after the wait returns, and a second program must not be copied
     * into them before the task is really gone.
     */
    g_program = NULL;

    task_exit(code);
}

/*
 * The board has no address space per task: a program is a task of the kernel
 * and its pointers are the kernel's own. Every range is accepted, exactly as
 * on a machine where the pointer check has nothing to check.
 */
bool k_port_exec_contains(const void *ptr, k_size_t size)
{
    K_UNUSED(ptr);
    K_UNUSED(size);
    return true;
}

/* Without an MMU the child shares the memory: the kernel's placeholder. */
int k_port_fork_copy(struct task *child)
{
    K_UNUSED(child);
    return -1;
}

/* The bounds of a vector of words in the data window. */
static bool vectors_fit(k_size_t end)
{
    return end <= PORT_EXEC_DATA_SIZE;
}

/*
 * Copy the image to the two windows, clear the bss, build the vectors of the
 * program and make the task that runs it. The windows are not written before
 * every check has passed, so a refused image leaves the machine as it was.
 */
int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    struct task *caller;
    struct task *program;
    struct port_exec_start *start;
    k_u8_t *text = (k_u8_t *)(uintptr_t)PORT_EXEC_TEXT_BASE;
    k_u8_t *data = (k_u8_t *)(uintptr_t)PORT_EXEC_DATA_BASE;
    k_size_t source;
    k_size_t data_end;
    k_size_t strings = 0;
    k_size_t env_strings = 0;
    k_size_t arg_off = 0;
    k_size_t array_off = 0;
    k_size_t arg_end = 0;
    k_size_t env_off = 0;
    k_size_t env_array_off = 0;
    k_size_t env_end = 0;
    k_size_t index;
    k_size_t user_argc = 0;
    k_size_t user_envc = 0;
    k_size_t user_argv = 0;
    k_size_t user_envp = 0;
    k_size_t entry = 0;
    int status = 0;
    int rc;

    caller = task_self();
    if (caller == NULL) {
        kerror("esp32: execve outside of a task");
        return -1;
    }

    if (image == NULL || image->data == NULL || image->text_size == 0) {
        return -1;
    }

    /* The image has to be whole, with the sums caught for a wrap. */
    source = (k_size_t)K_EXEC_HEADER_SIZE + image->text_size + image->data_size;
    if (source < image->text_size || source < image->data_size || source > image->size) {
        return -1;
    }

    if (image->entry < K_EXEC_HEADER_SIZE ||
        image->entry >= (k_size_t)K_EXEC_HEADER_SIZE + image->text_size) {
        return -1;
    }

    if (image->bss_size > K_EXEC_BSS_MAX) {
        return -1;
    }

    if (image->text_size > PORT_EXEC_TEXT_SIZE - K_EXEC_HEADER_SIZE) {
        kerror("esp32: the text of the program (%u bytes) does not fit the %u-byte window",
               (unsigned)image->text_size, (unsigned)PORT_EXEC_TEXT_SIZE);
        return -1;
    }

    if (image->data_size + image->bss_size > PORT_EXEC_DATA_SIZE) {
        kerror("esp32: the data of the program (%u + %u bytes) does not fit the %u-byte window",
               (unsigned)image->data_size, (unsigned)image->bss_size,
               (unsigned)PORT_EXEC_DATA_SIZE);
        return -1;
    }

    /*
     * The windows hold one program. A program that calls execve would have to
     * overwrite the code it is running from; the kernel turns this refusal
     * into ENOMEM and the shell can say so. The check and the claim are one
     * step under the lock of the port: the board has a shell on the UART and
     * one on telnet, and the two may ask at the same time.
     */
    k_port_enter_critical();
    if (g_window_busy) {
        k_port_exit_critical();
        kerror("esp32: the window of the programs is in use (one program at a time)");
        return -1;
    }
    g_window_busy = true;
    k_port_exit_critical();

    /* The arguments are copied into the window, not handed as the pointers of
     * the kernel: the program is a task of the machine and may read them. */
    if (argv != NULL) {
        while (user_argc < PORT_EXEC_ARGC_MAX && argv[user_argc] != NULL) {
            k_size_t length = (k_size_t)strlen(argv[user_argc]);

            if (strings + length + 1u > PORT_EXEC_ARG_BYTES_MAX) {
                return -1;
            }

            strings += length + 1u;
            user_argc++;
        }
    }

    if (image->envp != NULL) {
        while (user_envc < PORT_EXEC_ENVP_MAX && image->envp[user_envc] != NULL) {
            k_size_t length = (k_size_t)strlen(image->envp[user_envc]);

            if (env_strings + length + 1u > PORT_EXEC_ENV_BYTES_MAX) {
                return -1;
            }

            env_strings += length + 1u;
            user_envc++;
        }
    }

    /* The data, then the bss, then the strings and the arrays of the vectors.
     * The bss is cleared, so the program starts with zeros and not with the
     * bytes of the program that ran before. */
    data_end = align_up(image->data_size + image->bss_size, 4u);

    if (user_argc > 0) {
        arg_off = align_up(data_end, 4u);
        array_off = align_up(arg_off + strings, 4u);
        arg_end = array_off + (user_argc + 1u) * sizeof(k_u32_t);
    }

    if (image->envp != NULL) {
        env_off = align_up((user_argc > 0) ? arg_end : data_end, 4u);
        env_array_off = align_up(env_off + env_strings, 4u);
        env_end = env_array_off + (user_envc + 1u) * sizeof(k_u32_t);
    }

    if (!vectors_fit(arg_end) || !vectors_fit(env_end)) {
        kerror("esp32: the arguments or the environment do not fit the data window");
        return -1;
    }

    memcpy(text + K_EXEC_HEADER_SIZE, (const k_u8_t *)image->data + K_EXEC_HEADER_SIZE,
           image->text_size);
    memcpy(data, (const k_u8_t *)image->data + K_EXEC_HEADER_SIZE + image->text_size,
           image->data_size);
    memset(data + image->data_size, 0, image->bss_size);

    if (user_argc > 0) {
        k_size_t cursor = arg_off;

        for (index = 0; index < user_argc; index++) {
            k_size_t length = (k_size_t)strlen(argv[index]) + 1u;
            k_u32_t pointer = (k_u32_t)(PORT_EXEC_DATA_BASE + cursor);

            memcpy(data + cursor, argv[index], length);
            memcpy(data + array_off + index * sizeof(k_u32_t), &pointer, sizeof(pointer));

            cursor += length;
        }

        {
            k_u32_t terminator = 0;

            memcpy(data + array_off + user_argc * sizeof(k_u32_t), &terminator, sizeof(terminator));
        }

        user_argv = PORT_EXEC_DATA_BASE + array_off;
    }

    if (image->envp != NULL) {
        k_size_t cursor = env_off;

        for (index = 0; index < user_envc; index++) {
            k_size_t length = (k_size_t)strlen(image->envp[index]) + 1u;
            k_u32_t pointer = (k_u32_t)(PORT_EXEC_DATA_BASE + cursor);

            memcpy(data + cursor, image->envp[index], length);
            memcpy(data + env_array_off + index * sizeof(k_u32_t), &pointer, sizeof(pointer));

            cursor += length;
        }

        {
            k_u32_t terminator = 0;

            memcpy(data + env_array_off + user_envc * sizeof(k_u32_t), &terminator,
                   sizeof(terminator));
        }

        user_envp = PORT_EXEC_DATA_BASE + env_array_off;
    }

    /* The core may still hold the bytes of the program before this one in its
     * pipeline; the call of the entry must fetch the new text. */
    instruction_sync();

    start = (struct port_exec_start *)k_port_alloc(sizeof(*start), false);
    if (start == NULL) {
        g_window_busy = false;
        kerror("esp32: no memory for the start of a program");
        return -1;
    }

    start->entry = PORT_EXEC_TEXT_BASE + image->entry;
    start->argc = user_argc;
    start->argv = user_argv;
    start->envp = user_envp;
    entry = start->entry;

    /*
     * The program inherits the descriptors, the directory and the job of its
     * caller, as POSIX leaves them across an exec; the copy is made before
     * the task can run.
     */
    program = task_fork_shared(caller, PORT_EXEC_TASK_NAME, port_exec_program_task, start);
    if (program == NULL) {
        k_port_free(start);
        g_window_busy = false;
        return -1;
    }

    kinfo("esp32: the program is in the text at 0x%08x, the data at 0x%08x, entry 0x%08x",
          (unsigned)PORT_EXEC_TEXT_BASE, (unsigned)PORT_EXEC_DATA_BASE, (unsigned)entry);

    /*
     * The caller waits for the program the way a parent waits for a child.
     * Ctrl+C on the console of the foreground program is the one signal the
     * port can hand it; a key that is not Ctrl+C is dropped, as on x86-64.
     */
    {
        const bool own_console = (task_get_job(caller) == TASK_JOB_NONE);
        k_u32_t waited = 0;

        for (;;) {
            if (own_console) {
                int key = k_console_poll();

                if (key == 3) {
                    (void)task_signal_send(program->pid, K_SIGINT);
                }
            }

            rc = task_wait(program->pid, &status, PORT_EXEC_STEP_MS);
            if (rc != -2) {
                break;
            }

            waited += PORT_EXEC_STEP_MS;
            if (waited >= PORT_EXEC_WAIT_MS) {
                break;
            }
        }
    }

    if (rc != 0) {
        /*
         * A timeout leaves the program running and the windows with it: the
         * next program would overwrite the text under a live task. The
         * windows stay busy until the board is restarted, and the answer says
         * that the program did not end.
         */
        kerror("esp32: the program did not end (%d)", rc);
        return -1;
    }

    g_program = NULL;
    g_window_busy = false;

    k_exec_exit(status);
    kinfo("esp32: the program exited with %d", status & 0xFF);

    return 0;
}
