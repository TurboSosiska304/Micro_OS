/*
 * Micro OS — the configuration space of the PCI bus of the x86-64 port.
 *
 * Mechanism 1, the one every PC has had since the 440FX: a 32-bit address
 * written to 0xCF8 and the answer read from 0xCFC. The bus of the machine is
 * flat in QEMU, and a device under a bridge is a scan this port does not need
 * yet.
 *
 * Two drivers of the port read the space and neither is the owner of the
 * mechanism: the network finds its virtio card here and the screen finds the
 * VGA of QEMU (ADR 0023). The four registers and the two calls live in one
 * file so that the second driver is a user of the first one and not a second
 * copy of the same four lines.
 */
#ifndef MICRO_OS_PORT_X86_64_PCI_H
#define MICRO_OS_PORT_X86_64_PCI_H

#include "kernel/types.h"

/* One function of one slot of one bus, the address of a device. */
struct port_x86_64_pci_device {
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;
};

/* The registers of the configuration space a driver of this port reads. */
#define PCI_VENDOR 0x00u
#define PCI_DEVICE 0x02u
#define PCI_COMMAND 0x04u
#define PCI_REVISION 0x08u /* the programming interface is the second byte */
#define PCI_CLASS 0x0Au    /* the sub-class, then the class */
#define PCI_BAR0 0x10u
#define PCI_BAR1 0x14u /* the upper half of a 64-bit window of BAR0 */
#define PCI_BAR2 0x18u
#define PCI_BAR3 0x1Cu /* the upper half of a 64-bit window of BAR2 */
#define PCI_BAR4 0x20u /* the fifth window: the bus-master registers of IDE */

/* The two bits of the command register a device needs to answer at all. */
#define PCI_COMMAND_IO 0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u

/* The lowest bit of a base address register: set, the window is a range of
 * ports; clear, a range of memory. */
#define PCI_BAR_IO 0x00000001u

/* A read of 32 or 16 bits; `offset` is a byte offset and is taken to a word. */
k_u32_t port_x86_64_pci_read32(const struct port_x86_64_pci_device *device, k_u8_t offset);
k_u16_t port_x86_64_pci_read16(const struct port_x86_64_pci_device *device, k_u8_t offset);
void port_x86_64_pci_write32(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u32_t value);
void port_x86_64_pci_write16(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u16_t value);

/*
 * The first function of the machine whose identifiers are the pair, in the
 * order of the bus, the slot and the function. False when there is none: a
 * machine without the device is a machine without the driver, not an error.
 */
bool port_x86_64_pci_find(k_u16_t vendor, k_u16_t device, struct port_x86_64_pci_device *found);

/*
 * The first function of a class: the class and the sub-class are the two
 * bytes at 0x0B and 0x0A of the configuration space, and `prog_if` is a mask
 * of the bits of the programming interface that the function must have. A
 * driver that wants a bus master asks for class 01 (storage), sub-class 01
 * (IDE) and bit 7 of the interface - "the function can master the bus" - and
 * does not have to know the vendor of the chip that QEMU put there.
 *
 * False when there is none.
 */
bool port_x86_64_pci_find_class(k_u8_t base, k_u8_t sub, k_u8_t prog_if,
                                struct port_x86_64_pci_device *found);

/*
 * A base address register: `where` from the register itself (the low bits of
 * a memory window are zero, an I/O window is marked by bit 0), `size` from the
 * all-ones probe of the register. A driver that wants the framebuffer of the
 * VGA (ADR 0023) asks for a window that is at least the picture, because a
 * BAR says where the window is and the size is the device's answer.
 */
/* The size of the window a BAR asks for: the register is written with all
 * ones and read back, which is the way a PCI device has answered since the
 * bus was a bus. The old value is put back before the call returns. */
k_u32_t port_x86_64_pci_bar_size(const struct port_x86_64_pci_device *device, k_u8_t offset);

/* Let the device decode `bits` of the command register. */
void port_x86_64_pci_enable(const struct port_x86_64_pci_device *device, k_u16_t bits);

#endif /* MICRO_OS_PORT_X86_64_PCI_H */
