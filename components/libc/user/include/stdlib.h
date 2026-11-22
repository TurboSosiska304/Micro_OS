/*
 * Micro OS — the allocation and the conversions of a program.
 *
 * The names are the ones of the C library because a ported program includes
 * this header and not `ulibc.h`; the functions behind them are the arena of
 * `src/malloc.c`, which lives in the window of the program and never asks the
 * kernel for a block. The kernel's `KSYS_MALLOC` returns memory of ring 0, and
 * a program could not read a byte of it.
 */
#ifndef MICRO_OS_LIBC_USER_STDLIB_H
#define MICRO_OS_LIBC_USER_STDLIB_H

#include "ulibc.h"

#define EXIT_SUCCESS ULIBC_EXIT_SUCCESS
#define EXIT_FAILURE ULIBC_EXIT_FAILURE

#endif /* MICRO_OS_LIBC_USER_STDLIB_H */
