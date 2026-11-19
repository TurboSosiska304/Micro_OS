/*
 * Micro OS — the proof that the kernel links against the contract alone.
 *
 * The host port is a machine: it brings POSIX with it. This probe links the
 * kernel and the shell against the null port instead, so a symbol that the
 * portable code takes from the platform by accident is a link error here,
 * on a laptop, and not on a board that nobody has.
 *
 * It runs too: the shell registers its commands and the process ends. There
 * is no task to start, which is exactly what the null port says.
 */
#include "kshell/kshell.h"

int main(void)
{
    return kshell_init() == KSHELL_OK ? 0 : 1;
}
