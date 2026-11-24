/*
 * Micro OS — the x86-64 port: the SATA controller of a chipset (ADR 0041).
 *
 * The controller is a function of the PCI bus (class 01 storage, sub-class 06
 * SATA, the AHCI mode of the programming interface) with a window of
 * registers in the memory. The window sits above the identity map of the
 * first gigabyte on every machine of the port, so it is mapped into the top
 * 2 GiB the way the screen and the APIC are (ADR 0023), and the driver reads
 * the registers through that window.
 *
 * The disk a controller brings was not formatted by this system: the rule of
 * kernel/volume.h decides what may be written, and the answer of this step is
 * "nothing". The driver registers the volume read-only, the boot lists what it
 * found, and not one byte of the medium changes.
 */
#ifndef MICRO_OS_PORT_X86_64_AHCI_H
#define MICRO_OS_PORT_X86_64_AHCI_H

#include "kernel/types.h"

#include "port_x86_64.h"

/*
 * The window of the registers of the controller in the kernel's map: above
 * the screen (0x20000000) and the APIC (0x30000000), inside the 1 GiB of the
 * top half the table of the port maps. The address of the window is the page
 * the ABAR starts in, and the driver adds the offset of the ABAR to it.
 */
#define PORT_X86_64_AHCI_VIRT (PORT_X86_64_KERNEL_BASE + 0x34000000ull)

/*
 * Find the controller, bring the port with a disk up, read the identify data
 * and register the disk of the block layer as "ahci0" (read-only). A machine
 * without the function or without a disk returns at once: the port then boots
 * the medium it had.
 */
void port_x86_64_ahci_init(void);

/*
 * The first partition of the disk, which the boot mounts: NULL when the
 * machine has no AHCI disk or the disk has no table this kernel reads.
 */
const char *port_x86_64_ahci_root(void);

#endif /* MICRO_OS_PORT_X86_64_AHCI_H */
