/*
 * Micro OS — the I/O APIC of the x86-64 machine (ADR 0012).
 *
 * The stub of the branch. The step this file owns is the last of the
 * interrupt work: take the timer and the keyboard off the 8259 and route the
 * lines through the I/O APIC to the processor of the boot, so that a machine
 * whose firmware delivers nothing to the legacy chip still has a clock. The
 * 8259 path is the one every smoke test of the port exercises today; the
 * reroute is a change of its own, with its own evidence, and it lands after
 * the second core runs.
 */
#include "port_x86_64_apic.h"
