/*
 * Micro OS — the configuration space of the PCI bus of the x86-64 port.
 *
 * The mechanism is one pair of ports and one register: the address of a
 * function and of a word inside its configuration space goes out to 0xCF8,
 * and the word comes back from 0xCFC (or goes there). Everything a driver
 * asks for - the identifiers, the command register, a base address - is one
 * of the calls below, and no driver programs the pair itself.
 */
#include "port_x86_64_pci.h"

#include "port_x86_64.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

/* The enable bit of the address port, and the one bit of an I/O BAR: the
 * register says "this window is ports" with the lowest bit, and "this window
 * is memory" with a zero there. */
#define PCI_CONFIG_ENABLE 0x80000000u
#define PCI_BAR_IO 0x00000001u

static k_u32_t config_address(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    return PCI_CONFIG_ENABLE | ((k_u32_t)device->bus << 16) | ((k_u32_t)device->slot << 11) |
           ((k_u32_t)device->function << 8) | ((k_u32_t)offset & 0xFCu);
}

k_u32_t port_x86_64_pci_read32(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    port_outl(PCI_CONFIG_ADDRESS, config_address(device, offset));

    return port_inl(PCI_CONFIG_DATA);
}

k_u16_t port_x86_64_pci_read16(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    k_u32_t value = port_x86_64_pci_read32(device, offset);

    return (k_u16_t)((value >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

void port_x86_64_pci_write32(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u32_t value)
{
    port_outl(PCI_CONFIG_ADDRESS, config_address(device, offset));
    port_outl(PCI_CONFIG_DATA, value);
}

/*
 * A write of one register of 16 bits: the configuration space takes a whole
 * 32-bit word, so the other half of it is read first and put back as it was.
 */
void port_x86_64_pci_write16(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u16_t value)
{
    k_u32_t old = port_x86_64_pci_read32(device, offset);
    k_u32_t shift = (offset & 2u) * 8u;
    k_u32_t mask = 0xFFFFu << shift;

    port_x86_64_pci_write32(device, offset, (old & ~mask) | (((k_u32_t)value << shift) & mask));
}

bool port_x86_64_pci_find(k_u16_t vendor, k_u16_t device_id, struct port_x86_64_pci_device *found)
{
    struct port_x86_64_pci_device device = {0, 0, 0};

    for (device.bus = 0; device.bus < 1u; device.bus++) {
        for (device.slot = 0; device.slot < 32u; device.slot++) {
            for (device.function = 0; device.function < 8u; device.function++) {
                /* An absent function answers with all ones, which is neither
                 * of the identifiers of a device. */
                if (port_x86_64_pci_read16(&device, PCI_VENDOR) != vendor) {
                    continue;
                }

                if (port_x86_64_pci_read16(&device, PCI_DEVICE) != device_id) {
                    continue;
                }

                if (found != NULL) {
                    *found = device;
                }

                return true;
            }
        }
    }

    return false;
}

k_u32_t port_x86_64_pci_bar_size(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    k_u32_t old = port_x86_64_pci_read32(device, offset);
    k_u32_t mask;

    /* The bits the device does not decode read back as zero, so the mask of
     * all ones names the window: its size is the lowest bit of the mask.
     * An I/O BAR keeps its lowest two bits and a memory one its four. */
    port_x86_64_pci_write32(device, offset, 0xFFFFFFFFu);
    mask = port_x86_64_pci_read32(device, offset);
    port_x86_64_pci_write32(device, offset, old);

    mask &= (old & PCI_BAR_IO) != 0u ? ~0x03u : ~0x0Fu;

    if (mask == 0u) {
        return 0;
    }

    return (~mask) + 1u;
}

void port_x86_64_pci_enable(const struct port_x86_64_pci_device *device, k_u16_t bits)
{
    k_u16_t command = port_x86_64_pci_read16(device, PCI_COMMAND);

    port_x86_64_pci_write16(device, PCI_COMMAND, (k_u16_t)(command | bits));
}
