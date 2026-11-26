/*
 * Micro OS — a session: a terminal, a person, and the shell of the kernel.
 *
 * The console of this machine is not a getty that a program answers: it is a
 * task that reads a line and runs it. 1.6 made `login` a command that moves
 * the credentials of the shell that runs it (ADR 0033), which is the door of
 * the console and, as the cleanroom of 1.6 found, a defect over the air:
 * `login` asked for the password with `k_console_getchar()` and a person at
 * the other end of a telnet connection typed into the UART of the board.
 *
 * This file is the answer of 1.8. A session is a task of the kernel with a
 * terminal of its own (kernel/console.h): the console of the machine, whose
 * bytes are the UART, or a client of telnet, whose bytes are a socket. The
 * dialogue of the door, the lines of the shell and the commands that ask a
 * question all read the terminal of the task they run in, so the password of
 * a session comes from the terminal that person is on and from nowhere else.
 *
 * A session cannot return to root by itself: this system has one credential
 * per task and no saved uid (ADR 0005, ADR 0033). So the init of every port
 * is the getty: it makes a session, waits for it, and makes another one when
 * a `logout` ended it. The console starts as root without a password - the
 * person in front of the machine is the machine - and a person who logged in
 * as somebody else gets the root prompt back by leaving the session, the way
 * a getty does it. `who` and /proc/sessions name the sessions that are in.
 */
#ifndef MICRO_OS_CONFIG_SESSION_H
#define MICRO_OS_CONFIG_SESSION_H

#include "config/users.h"
#include "kernel/console.h"
#include "kernel/task.h"
#include "kernel/types.h"

/* The line of a terminal: the line of the shell, and the room a password of
 * this system has. A line that does not fit is thrown away rather than cut,
 * because half a command is a different command. */
#define SESSION_LINE_MAX 128

/* The attempts of a password at the door, as ADR 0033 promises: three and
 * then the terminal is left. The wait after a refusal makes a dictionary
 * over a socket cost a second an answer; it is a delay and not a sleep of
 * the machine, because only the session waits. */
#define SESSION_ATTEMPTS 3
#define SESSION_FAIL_DELAY_MS 1500u

/* How a session ended, which is what the init that waits for it has to
 * know: another session may start after a logout, and nothing starts after
 * a terminal that closed or three wrong answers. */
#define SESSION_EXIT_LOGOUT 0
#define SESSION_EXIT_CLOSED 1
#define SESSION_EXIT_DENIED 2

/* The terminal and the kind of session that runs on it, as the server that
 * makes a session hands it over. The structure is copied by session_start(),
 * so the caller's copy may die at once; `terminal.ctx` is the state of the
 * caller and outlives the session (a socket, a script of a test). */
struct session_info {
    struct k_terminal terminal;
    k_u8_t tty; /* TASK_TTY_CONSOLE or TASK_TTY_TELNET */
};

/* The bytes of the server on the terminal of the calling task. */
void session_write(const char *text);
void session_write_fmt(const char *fmt, ...);

/*
 * One line of the terminal of the calling task. The prompt is written first
 * (nothing when it is NULL), every byte is echoed through the terminal
 * unless `secret` is true or the terminal asked for no echo, backspace
 * erases a character, Ctrl+C drops the line (and closes a secret one), and
 * the answer ends with a newline. False when the terminal is gone; a line
 * that did not fit is thrown away and `too_long` says so. A secret line also
 * mutes the machine's file log for this task from the prompt to the answer,
 * so an echo by the port cannot put the password in `run.log`.
 */
bool session_read_line(const char *prompt, char *buf, k_size_t size, bool secret, bool *too_long);

/*
 * The door of a session of the network: the name (unless it is given) and
 * the password, SESSION_ATTEMPTS times, with one sentence for a name that
 * does not exist and a password that is not the one - a hint about which of
 * the two was wrong is a list of the accounts of the machine - and a wait of
 * `delay_ms` before each sentence. USERS_OK with the account, USERS_EAUTH
 * when the three answers were wrong, USERS_EIO when the files are not
 * readable. The password is never written anywhere and is erased from its
 * buffer after every attempt.
 */
int session_authenticate(const char *given, struct user_account *out, k_u32_t delay_ms);

/*
 * The lines of the shell on the terminal of the calling task, until `logout`
 * or the end of the terminal. It is the loop `kshell_run()` used to be, with
 * the terminal of the task instead of the console of the machine.
 */
int session_run(void);

/*
 * The task of a session: the door (the console starts as root and asks
 * nothing; a session of telnet authenticates and enters the home), then the
 * loop. It always exits with one of SESSION_EXIT_*.
 */
void session_task(void *arg);

/*
 * Make a session as the caller - the caller's credentials are the ones the
 * session starts with - and answer its pid, or 0 when there is no room. The
 * terminal of a console session is left empty: the port is the console.
 */
k_u32_t session_start(const struct session_info *info);

/*
 * The console session, for the init of a port: start the first one, and give
 * it another one when a reaped child was the console and ended with a
 * logout. `session_console_reaped()` is called from the reaper loop of init
 * for every child it reaps; a child that is not the console is left alone.
 */
k_u32_t session_console_start(void);
void session_console_reaped(k_u32_t pid, int status);

/* Register `logout`, `who` and `whoami` with the shell. */
int session_register(void);

#endif /* MICRO_OS_CONFIG_SESSION_H */
