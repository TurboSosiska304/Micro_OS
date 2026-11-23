/*
 * Micro OS — the status of a child, for the One True Awk.
 *
 * `system()` of this machine answers -1 and never a status, because there is
 * no shell and no second process (`port/awk_os.c`), so the macros of the
 * source are compiled and never consulted. They are the POSIX shape and not a
 * second convention: a wait status of this system is the one of its kernel
 * (`kernel/ksys.h`), and a port with a shell would hand the same bits.
 */
#ifndef MICRO_OS_THIRD_PARTY_AWK_PORT_SYS_WAIT_H
#define MICRO_OS_THIRD_PARTY_AWK_PORT_SYS_WAIT_H

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) ((((status) & 0x7f) != 0) && (((status) & 0x7f) != 0x7f))
#define WTERMSIG(status) ((status) & 0x7f)

#endif /* MICRO_OS_THIRD_PARTY_AWK_PORT_SYS_WAIT_H */
