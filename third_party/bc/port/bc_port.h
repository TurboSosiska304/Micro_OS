/*
 * Micro OS — the configuration of bc 7.2.0 for a program of the system.
 *
 * The release is configured by the flags of `configure.sh` and not by a
 * `config.h`: `Makefile.in` passes the settings of the build on the command
 * line. This header is force-included into every source of the library (and
 * of the program) and names the same settings, so the sources of the release
 * stay unchanged and the next update is a copy of its files.
 *
 * The settings are the ones the recipe of a port uses:
 *
 *   - the two calculators in one image, as the release does: `main` reads
 *     the name it was entered with and keeps the engine that belongs to it
 *     (`bc` or `dc`);
 *   - the extra math of the release (`$`, `@`, `irand`, ...), GNU's default;
 *   - no history and no line editor: the console of this machine has no
 *     terminal behind descriptor 0, and the port does not carry readline,
 *     editline or the internal line editor (`docs/debts.md`);
 *   - no NLS: the messages are the strings of the release;
 *   - no library (`bcl`) and no fuzzer;
 *   - a 64-bit limb: the machine is x86-64 and its `long` is 64 bits. GCC's
 *     freestanding `<limits.h>` does not define `LONG_BIT`, and the release
 *     would fall back to 32 bits without a word from us.
 *
 * The signal and the stat shapes of the machine are below. This system has
 * four signals and no `sigaction`, and its `stat` is a call of a path and not
 * of a descriptor (`third_party/bc/port/bc_os.c`). The two shims are for the
 * program of the machine only: the host test builds the same sources with
 * the headers of the host and asks its C library for the two calls.
 */
#ifndef MICRO_OS_BC_PORT_H
#define MICRO_OS_BC_PORT_H

#define VERSION 7.2.0
#define BUILD_TYPE N

#define BC_ENABLED 1
#define DC_ENABLED 1
#define BC_ENABLE_EXTRA_MATH 1
#define BC_ENABLE_HISTORY 0
#define BC_ENABLE_NLS 0
#define BC_ENABLE_LIBRARY 0
#define BC_ENABLE_MEMCHECK 0
#define BC_ENABLE_AFL 0
#define BC_ENABLE_OSSFUZZ 0
#define BC_ENABLE_EDITLINE 0
#define BC_ENABLE_READLINE 0
#define BC_NUM_KARATSUBA_LEN 32

/* The defaults of `configure.sh -O2`: the banner is off, the prompt is the
 * one of an interactive calculator that this machine never reaches (a
 * descriptor of the console is not a terminal), and an expression ends the
 * program as the release does. */
#define BC_DEFAULT_BANNER 0
#define BC_DEFAULT_SIGINT_RESET 1
#define BC_DEFAULT_TTY_MODE 1
#define BC_DEFAULT_PROMPT 1
#define BC_DEFAULT_EXPR_EXIT 1
#define BC_DEFAULT_DIGIT_CLAMP 0
#define DC_DEFAULT_SIGINT_RESET 1
#define DC_DEFAULT_TTY_MODE 0
#define DC_DEFAULT_PROMPT 0
#define DC_DEFAULT_EXPR_EXIT 1
#define DC_DEFAULT_DIGIT_CLAMP 0

#define NDEBUG 1

/* The limbs of the release's number: `LONG_BIT` is a fact of the machine,
 * and the freestanding header of the compiler does not carry it. */
#ifndef LONG_BIT
#define LONG_BIT 64
#endif
#ifndef BC_LONG_BIT
#define BC_LONG_BIT 64
#endif

#ifndef BC_PORT_HOST

/*
 * The machine has `signal()` and no `sigaction` (kernel/signal.h): the
 * release installs its handler with the POSIX call of the structure. The
 * wrapper in `bc_os.c` turns the structure into the call the system has;
 * the signals it does not know are refused and the release ignores that
 * answer, as it does with the disposition of a signal it cannot set.
 */
struct sigaction {
    void (*sa_handler)(int);
    unsigned long sa_mask;
    int sa_flags;
};

#define sigemptyset(set) ((void)(set))
#define SA_NODEFER 0

int sigaction(int number, const struct sigaction *action, struct sigaction *old);

/*
 * The release's error path is a `sigsetjmp`/`siglongjmp`, which POSIX keeps
 * beside the plain pair; the library of this program has the plain pair and
 * nothing else (components/libc/user/include/setjmp.h).
 */
#define sigjmp_buf jmp_buf
#define sigsetjmp(environment, save) setjmp(environment)
#define siglongjmp(environment, value) longjmp(environment, value)

/* The two names the release asks of `<signal.h>` and this machine does not
 * have: the kernel's four signals are in `signal.h` of the library. */
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif

#endif /* !BC_PORT_HOST */

#endif /* MICRO_OS_BC_PORT_H */
