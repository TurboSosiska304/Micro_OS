/*
 * Micro OS — the x86-64 port: the ethernet of a laptop, an Intel I219 (the
 * family of the `e1000e` of its driver of Linux).
 *
 * The card is a function of the PCI bus with one memory window of 128 KiB in
 * BAR0: the control registers, the two rings of descriptors and the counters
 * live behind it. The window sits above the identity map of the first
 * gigabyte on every machine of the port, so it is mapped into the top 2 GiB
 * the way the screen, the APIC, the SATA controller and the NVMe disk are
 * (ADR 0023, ADR 0041, ADR 0042), and the driver reads the card through that
 * window.
 *
 * e1000e and not the wire of the USB of 1.8 (ADR 0055): the card is the one
 * a laptop has, QEMU models it (`-device e1000e`) so the gate is a machine,
 * and the RTL8153 needs a driver of its own with a firmware table and a
 * vendor protocol, while the device of the USB of this port speaks CDC-ECM
 * and not the RTL.
 */
#ifndef MICRO_OS_PORT_X86_64_E1000E_H
#define MICRO_OS_PORT_X86_64_E1000E_H

#include "kernel/types.h"

#include "port_x86_64.h"

/*
 * The window of the registers of the card in the kernel's map: above the
 * screen (0x20000000), the APIC (0x30000000), the USB controller
 * (0x32000000), the SATA controller (0x34000000) and the NVMe disk
 * (0x36000000), inside the 1 GiB of the top half the table of the port maps.
 * The window is one 2 MiB page and the driver adds the offset of BAR0 inside
 * its first page to it.
 */
#define PORT_X86_64_E1000_VIRT (PORT_X86_64_KERNEL_BASE + 0x38000000ull)

/*
 * Find the card, reset it, read the MAC out of its NVM, build the two rings
 * and register the device as "e1000e0" under the stack. A machine without
 * the function returns at once: the port then boots with the card of virtio
 * or with no network at all, and neither is an error of the boot.
 */
void port_x86_64_e1000e_init(void);

/* The name the driver registered - "e1000e0" - or NULL when it did not. */
const char *port_x86_64_e1000e_name(void);

#endif /* MICRO_OS_PORT_X86_64_E1000E_H */
