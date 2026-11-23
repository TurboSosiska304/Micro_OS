/*
 * Micro OS — the one declaration the port of sbase adds to the upstream files.
 *
 * `sort -n` compares two numbers as reals, and the port of a program of this
 * system is built without the registers of the floating point unit: the kernel
 * saves no XMM state and the flags of the guest say `-mgeneral-regs-only`, so
 * a `double` in a function of the guest does not compile. The comparator below
 * reads the two numbers as decimals and orders them without a real number; it
 * is exact for the sizes a line of a file holds and never rounds.
 *
 * The header is force-included in every file of sbase (`-include
 * port/sbase_port.h`), so the one patched line of `sort.c` sees the
 * declaration without a second edit of an upstream header.
 */
#ifndef MICRO_OS_THIRD_PARTY_SBASE_PORT_SBASE_PORT_H
#define MICRO_OS_THIRD_PARTY_SBASE_PORT_SBASE_PORT_H

/* -1, 0 or 1 for two decimal numbers of the text, as `sort -n` orders them. */
int sbase_numcmp(const char *left, const char *right);

#endif /* MICRO_OS_THIRD_PARTY_SBASE_PORT_SBASE_PORT_H */
