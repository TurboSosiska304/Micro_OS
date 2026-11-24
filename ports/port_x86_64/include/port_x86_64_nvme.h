/*
 * Micro OS — the x86-64 port: the disk of a modern laptop, on NVMe (ADR 0042).
 *
 * The controller is a function of the PCI bus (class 01 storage, sub-class 08
 * NVMe) with a 64-bit window of registers in BAR0. The window sits above the
 * identity map of the first gigabyte on every machine of the port, so it is
 * mapped into the top 2 GiB the way the screen, the APIC and the SATA
 * controller are (ADR 0023, ADR 0041), and the driver reads the registers
 * through that window.
 *
 * The disk a controller brings was not formatted by this system: the rule of
 * kernel/volume.h decides what may be written, and the answer of this step is
 * "nothing" - there is no WRITE command in the driver. The boot lists what it
 * found, and not one byte of the medium changes.
 */
#ifndef MICRO_OS_PORT_X86_64_NVME_H
#define MICRO_OS_PORT_X86_64_NVME_H

#include "kernel/types.h"

#include "port_x86_64.h"

/*
 * The window of the registers of the controller in the kernel's map: above
 * the screen (0x20000000), the APIC (0x30000000), the USB controller
 * (0x32000000) and the SATA controller (0x34000000), inside the 1 GiB of the
 * top half the table of the port maps. The address of the window is the page
 * BAR0 starts in, and the driver adds the offset of BAR0 to it.
 */
#define PORT_X86_64_NVME_VIRT (PORT_X86_64_KERNEL_BASE + 0x36000000ull)

/*
 * Find the controller, bring the admin queue up, identify the controller and
 * the namespace, create the one I/O queue pair and register the disk of the
 * block layer as "nvme0" (read-only). A machine without the function or
 * without a namespace returns at once: the port then boots the medium it had.
 */
void port_x86_64_nvme_init(void);

/*
 * The first partition of the disk, which the boot mounts: NULL when the
 * machine has no NVMe disk or the disk has no table this kernel reads.
 */
const char *port_x86_64_nvme_root(void);

#endif /* MICRO_OS_PORT_X86_64_NVME_H */
