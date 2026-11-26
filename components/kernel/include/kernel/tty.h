/*
 * Micro OS — the terminal of a program.
 *
 * A terminal of this system is `/dev/tty0` (and `/dev/tty`, the name POSIX
 * gives the same device): the console of the machine for a task that has no
 * terminal of its own, and the terminal of the task for a session of telnet
 * (ADR 0051). What travels through here is the part of a terminal that is not
 * the bytes: the mode of the line (canonical or raw, the echo, the signal of
 * Ctrl+C), the size of the screen in cells, and the two questions a program
 * asks about them, `termios` and `ioctl`.
 *
 * The mode belongs to the terminal and not to the descriptor, as it does on a
 * real machine: two programs that open one console share its mode, and the
 * console is put back to canonical when the program that left it raw goes
 * away. The structure of a task that has a terminal of its own is made on the
 * first question and freed with the task.
 *
 * Nothing here is hardware: the bytes come from `kernel/console.h` and the
 * size from `kernel/port/console.h`, and the board, the host process and
 * x86-64 answer with the console each of them has.
 */
#ifndef MICRO_OS_KERNEL_TTY_H
#define MICRO_OS_KERNEL_TTY_H

#include "kernel/types.h"

struct task;

/*
 * The requests of `ioctl` a terminal answers. The numbers are the ones of
 * Linux: a program does not compare them to constants, it names them, but a
 * source that does hardcode one is not silently wrong. A device that does not
 * know a request answers EINVAL (ADR 0028, ADR 0051 §3).
 */
#define K_TTY_TCGETS 0x5401u
#define K_TTY_TCSETS 0x5402u
#define K_TTY_TCSETSW 0x5403u /* the same as TCSETS: the output is never queued */
#define K_TTY_TCSETSF 0x5404u /* set the mode, then drop the input */
#define K_TTY_TIOCGPGRP 0x540Fu  /* foreground process group of the tty */
#define K_TTY_TIOCSPGRP 0x5410u  /* set foreground process group of the tty */
#define K_TTY_TIOCGWINSZ 0x5413u
#define K_TTY_TIOCSWINSZ 0x5414u /* accepted and ignored: nothing resizes a machine */
#define K_TTY_FIONREAD 0x541Bu
#define K_TTY_TIOCGETA 0x5401u /* the BSD name of TCGETS, the same call */

/* The bytes of `c_cc`, the Linux number. */
#define K_TTY_NCCS 19u

/* The mode of a line, as it crosses the ABI: 36 bytes with no padding, so the
 * `struct termios` of the library is this structure and not a translation of
 * it. The flags and the indices of `c_cc` are the ones of Linux; the ones
 * this system implements are in the comment of each. */
struct k_tty_termios {
    k_u32_t c_iflag; /* ICRNL applied; IXON, IXOFF, INPCK, ISTRIP carried */
    k_u32_t c_oflag; /* OPOST, ONLCR carried; the console breaks lines itself */
    k_u32_t c_cflag; /* CS8 and the speeds are no-ops: the line is fixed */
    k_u32_t c_lflag; /* ICANON, ECHO, ECHOE, ISIG are implemented */
    k_u8_t c_line;
    k_u8_t c_cc[K_TTY_NCCS];
};

/* The size of a terminal in cells, the `struct winsize` of the ABI. */
struct k_tty_winsize {
    k_u16_t ws_row;
    k_u16_t ws_col;
    k_u16_t ws_xpixel;
    k_u16_t ws_ypixel;
};

/* The indices of `c_cc` (Linux's, as the flags are). */
#define K_TTY_VINTR 0u
#define K_TTY_VQUIT 1u
#define K_TTY_VERASE 2u
#define K_TTY_VKILL 3u
#define K_TTY_VEOF 4u
#define K_TTY_VTIME 5u
#define K_TTY_VMIN 6u
#define K_TTY_VSWTC 7u
#define K_TTY_VSTART 8u
#define K_TTY_VSTOP 9u
#define K_TTY_VSUSP 10u
#define K_TTY_VEOL 11u
#define K_TTY_VREPRINT 12u
#define K_TTY_VDISCARD 13u
#define K_TTY_VWERASE 14u
#define K_TTY_VLNEXT 15u
#define K_TTY_VEOL2 16u

/* The bits of the four flags that this system reads. The numbers are the ones
 * of Linux so that a source of somebody else compiles against this header and
 * asks the question it meant to ask. */
#define K_TTY_IGNBRK 0000001u
#define K_TTY_BRKINT 0000002u
#define K_TTY_IGNPAR 0000004u
#define K_TTY_PARMRK 0000010u
#define K_TTY_INPCK 0000020u
#define K_TTY_ISTRIP 0000040u
#define K_TTY_INLCR 0000100u
#define K_TTY_IGNCR 0000200u
#define K_TTY_ICRNL 0000400u
#define K_TTY_IXON 0002000u
#define K_TTY_IXANY 0004000u
#define K_TTY_IXOFF 0010000u
#define K_TTY_IMAXBEL 0020000u
#define K_TTY_IUTF8 0040000u

#define K_TTY_OPOST 0000001u
#define K_TTY_ONLCR 0000004u
#define K_TTY_OCRNL 0000010u
#define K_TTY_ONOCR 0000020u
#define K_TTY_ONLRET 0000040u

#define K_TTY_CSIZE 0000060u
#define K_TTY_CS8 0000060u
#define K_TTY_CSTOPB 0000100u
#define K_TTY_CREAD 0000200u
#define K_TTY_PARENB 0000400u
#define K_TTY_CLOCAL 0004000u

#define K_TTY_ISIG 0000001u
#define K_TTY_ICANON 0000002u
#define K_TTY_ECHO 0000010u
#define K_TTY_ECHOE 0000020u
#define K_TTY_ECHOK 0000040u
#define K_TTY_ECHONL 0000100u
#define K_TTY_NOFLSH 0000200u
#define K_TTY_TOSTOP 0000400u
#define K_TTY_ECHOCTL 0001000u
#define K_TTY_ECHOKE 0004000u
#define K_TTY_IEXTEN 0100000u

/* The actions of tcsetattr. Every one of them sets the mode now: the output
 * of this system is never queued, and only TCSAFLUSH drops the input. */
#define K_TTY_TCSANOW 0
#define K_TTY_TCSADRAIN 1
#define K_TTY_TCSAFLUSH 2

/* The default control characters, the ones of a terminal of a Unix. */
#define K_TTY_DEF_INTR 3u    /* ^C */
#define K_TTY_DEF_QUIT 28u   /* ^\ */
#define K_TTY_DEF_ERASE 127u /* DEL */
#define K_TTY_DEF_KILL 21u   /* ^U */
#define K_TTY_DEF_EOF 4u     /* ^D */
#define K_TTY_DEF_START 17u  /* ^Q */
#define K_TTY_DEF_STOP 19u   /* ^S */
#define K_TTY_DEF_SUSP 26u   /* ^Z */

/* The longest line of the canonical mode. It is the line of the shell: a
 * program that reads a line of a person reads one that fits a command. */
#define K_TTY_LINE_MAX 128u

/*
 * Read from the terminal of the calling task. Returns the number of bytes
 * (zero at the end of the file or when a canonical line is empty and Ctrl+D
 * ended it), or a negative error code. The call sleeps while the terminal is
 * quiet and is woken by the key.
 */
int k_tty_read(char *buf, k_size_t size);

/*
 * The `ioctl` of the terminal: the requests of the table above, answered for
 * the calling task. `arg` is a pointer of the program and is checked before
 * it is written (kernel/port/exec.h).
 */
int k_tty_ioctl(k_u32_t request, k_u64_t arg);

/*
 * Whether the console of the port is being read by a program right now, which
 * is the question the caller of `execve` asks before it takes a key for
 * Ctrl+C (ADR 0051 §8).
 */
bool k_tty_console_claimed(void);

/*
 * Whether a byte waits at the terminal of the calling task, without taking
 * it: the answer of `poll`, which must not eat the key it reports. A byte
 * found here is kept for the read that follows.
 */
bool k_tty_input_ready(void);

/* Put the console back to canonical and release its reader: the task that
 * changed the mode is going away (called by task_free_slot). */
void k_tty_task_exit(struct task *t);

/* The mode of the console of the machine: canonical with the echo, which is
 * what the boot gives it. Called once, from the subsystem of the scheduler. */
void k_tty_init(void);

/* True when the mode of the console is raw: /proc and the tests read it. */
bool k_tty_console_raw(void);

/* Ensure the calling task's tty is in canonical mode for line reading. */
void k_tty_set_canonical(void);

/* Open/close of the console tty device: tracks session owner and open count.
 * Opening makes the calling task a member of the tty session; closing the
 * last descriptor releases the session. */
void k_tty_open(void);
void k_tty_close(void);

/* Foreground process group of the console tty (0 when none). */
k_u32_t k_tty_fg_pgrp(void);
int k_tty_set_fg_pgrp(k_u32_t pgid);

/* The mode the tty of the machine starts in, and the one `cfmakeraw` makes.
 * They are here and not in the library because the kernel gives a fresh task
 * the console's defaults and a test checks them without a program. */
void k_tty_defaults(struct k_tty_termios *out);
void k_tty_make_raw(struct k_tty_termios *settings);

#endif /* MICRO_OS_KERNEL_TTY_H */
