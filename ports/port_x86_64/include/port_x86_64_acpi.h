/*
 * Micro OS — the ACPI tables of the x86-64 port: the root pointer, the
 * tables below it and the two things the power of the machine needs.
 *
 * The processors of the machine are read from the MADT (src/cpu.c), which
 * owns the root pointer and the walk of RSDT/XSDT. This header is the half
 * of that walk the rest of the port asks about - a table by signature - and
 * the two calls of the FADT: S5 (the soft off of ACPI) and the reset
 * register of a machine that has one.
 *
 * Nothing here is required for the machine to run: a PC without a root
 * pointer falls back to the two ports QEMU and Bochs answer (src/acpi.c),
 * and a machine with neither is stopped by the caller with a sentence for
 * the person in front of it.
 */
#ifndef MICRO_OS_PORT_X86_64_ACPI_H
#define MICRO_OS_PORT_X86_64_ACPI_H

#include "kernel/types.h"

/*
 * The ACPI table with this four-letter signature, or NULL when the machine
 * has no root pointer, no such table or a table whose checksum is wrong.
 * `length` may be NULL; when it is not, it receives the length of the
 * table from its header. The pointer is a physical address of the identity
 * map of the port: the low gigabyte is mapped, which is where the firmware
 * keeps these tables.
 *
 * The signature is four characters and not a string: "FACP" is the FADT,
 * "APIC" is the MADT and a caller that writes "FACP\0" reads nothing.
 */
const void *port_x86_64_acpi_table(const char signature[4], k_u32_t *length);

/*
 * Switch the machine off with the S5 of ACPI: the FADT names the control
 * block of PM1a (and PM1b when it exists), and the write of SLP_TYPa with
 * SLP_EN is the request. Returns 0 when the write went out, -1 when the
 * machine has no FADT or the block is where this port cannot reach it. A
 * machine that obeys the write does not return from it at all: the 0 is
 * the answer of a machine that is still here a moment later.
 */
int port_x86_64_acpi_off(void);

/*
 * Reset the machine with the reset register of the FADT (ACPI 6.x, 4.8.3.6)
 * or with the reset control port of the chipset (0xCF9). Returns 0 when the
 * write went out, -1 when neither exists. Like the call above, a machine
 * that obeys does not return.
 */
int port_x86_64_acpi_reset(void);

/*
 * The sentence for the log: which of the two mechanisms the machine has,
 * "acpi s5", "qemu", "bochs" or "none". A person who reads the console of a
 * machine that does not go off needs to know what was tried.
 */
const char *port_x86_64_acpi_name(void);

#endif /* MICRO_OS_PORT_X86_64_ACPI_H */
