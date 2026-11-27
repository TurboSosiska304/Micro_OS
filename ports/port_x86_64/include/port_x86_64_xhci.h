/*
 * Micro OS — the USB 3.0 controller of the x86-64 port (ADR 0040).
 *
 * The machine of a laptop of these years has no PS/2 port the firmware will
 * share and no COM port, so the keyboard of the person in front of the screen
 * arrives through the host controller of the USB (xHCI, PCI class 0x0C,
 * sub-class 0x03, programming interface 0x30). This header is what the rest
 * of the port knows about it: a boot call, the characters of the console, the
 * events of `/dev/input0` and the capture of the keyboard by an interface.
 * Since the second iteration of M1 the controller enumerates the devices
 * behind its hubs, sees a device that arrives after the boot, reads the mouse
 * of the boot protocol and owns a line of its own (MSI/MSI-X, or the legacy
 * pin where the port keeps the 8259).
 *
 * The controller is optional: a machine without one keeps the keyboard of
 * the PS/2 and the boot says nothing, and every call below answers "nothing"
 * when there is no controller or no keyboard on it.
 */
#ifndef MICRO_OS_PORT_X86_64_XHCI_H
#define MICRO_OS_PORT_X86_64_XHCI_H

#include "kernel/input.h"
#include "kernel/types.h"

/*
 * The window of the controller in the high half, 2 MiB aligned as the map of
 * a device wants it. The three windows of the port that are taken are the
 * screen (0x20000000), the local APIC (0x30000000) and the I/O APIC
 * (0x30020000), so this one starts past them.
 */
#define PORT_X86_64_XHCI_VIRT (PORT_X86_64_KERNEL_BASE + 0x32000000ull)

/*
 * Find the controller on the PCI bus, map its registers, reset it and bring
 * up the keyboard of every port. Called once by port_x86_64_init() after the
 * table of the interrupts exists, because the waits of the enumeration are
 * measured by the clock of the machine. A machine without the controller is
 * a machine that boots as it did before.
 */
void port_x86_64_xhci_init(void);

/* True when a keyboard of the USB answered the enumeration and reports. */
bool port_x86_64_usb_keyboard(void);

/* True when a mouse of the boot protocol answered the enumeration. Its
 * events are the pointer events of `/dev/input0`, as the ones of the PS/2
 * are. */
bool port_x86_64_usb_pointer(void);

/* The next character of the USB keyboard, or -1 when its ring is empty. The
 * call also reads the controller, so a caller that polls the console often
 * is also the caller that moves the reports of the keyboard. */
int port_x86_64_usb_poll(void);

/* The next event of the USB keyboard, or false when its ring is empty. */
bool port_x86_64_usb_event(struct input_event *out);

/* The keyboard belongs to an interface while it is open: the characters of
 * the console stop and only the events are pushed, as the PS/2 does. */
void port_x86_64_usb_capture(bool on);

/* --- the classes on top of the controller (ADR 0040, M1 of 1.8) ----------- */

/*
 * A device a class driver drives. The class driver - `src/usb_storage.c` for
 * a stick of the class 08/06/50, `src/usb_net.c` for a wire of the class
 * 02/06, `src/usb_bt.c` for a Bluetooth controller of the class E0/01/01 -
 * is handed one of these by the enumeration and asks the controller for the
 * transfers of its class through the calls below. It never sees a ring, a
 * TRB or a doorbell.
 */
struct port_x86_64_usb_device {
    k_u8_t slot;
    k_u8_t port;
    k_u32_t speed; /* the code of PORTSC, 1 full to 4 SuperSpeed */
    k_u16_t vendor;
    k_u16_t product;
    k_u8_t config;    /* the bConfigurationValue the device was told to take */
    k_u8_t interface; /* the interface of the class */
    k_u8_t bulk_in_dci;
    k_u8_t bulk_out_dci;
    k_u16_t bulk_in_packet;
    k_u16_t bulk_out_packet;
    k_u8_t interrupt_in_dci;
    k_u16_t interrupt_in_packet;
    k_u8_t interrupt_in_interval;
};

/* The configuration a class driver reads while it attaches: the bytes of the
 * descriptor, its length and the value that selects it. */
struct port_x86_64_usb_config {
    const k_u8_t *bytes;
    k_u32_t length;
    k_u8_t value;
};

/*
 * The attach of a class, called by the enumeration for the device whose
 * configuration carries the class. The class reads the descriptors it needs
 * (its interface, its endpoints, its functional descriptor), asks the
 * controller to turn its endpoints on (`port_x86_64_xhci_endpoint`) and
 * registers its disk or its network device. False is a device the class
 * could not bring up: the enumeration gives the slot back.
 */
bool port_x86_64_usb_storage_attach(struct port_x86_64_usb_device *device,
                                    const struct port_x86_64_usb_config *config);
bool port_x86_64_usb_net_attach(struct port_x86_64_usb_device *device,
                                const struct port_x86_64_usb_config *config);
bool port_x86_64_usb_bt_attach(struct port_x86_64_usb_device *device,
                               const struct port_x86_64_usb_config *config);

/*
 * The device left (a hub port lost it, or the controller did): the class
 * forgets it and gives back what it registered - `usb0` leaves the table of
 * the block layer and its partitions with it. The device is only the one the
 * call names, so a class that is handed a stale view does nothing.
 */
void port_x86_64_usb_storage_detach(struct port_x86_64_usb_device *device);
void port_x86_64_usb_net_detach(struct port_x86_64_usb_device *device);
void port_x86_64_usb_bt_detach(struct port_x86_64_usb_device *device);

/*
 * Turn one endpoint of the device on: its address is the one of the
 * descriptor (0x81 for the endpoint 1 IN), `bulk` says whether the endpoint
 * is bulk or interrupt, and the context of the endpoint is programmed with
 * `max_packet` and the interval of the descriptor. The controller keeps the
 * endpoints that are already on and adds this one, as the specification
 * asks. The answer is 0 or a negative code.
 */
int port_x86_64_xhci_endpoint(struct port_x86_64_usb_device *device, k_u8_t address, bool bulk,
                              k_u8_t interval, k_u16_t max_packet);

/* One control transfer of the endpoint 0 of the device - a class request of
 * the driver (SET_INTERFACE, SET_ETHERNET_PACKET_FILTER) or a descriptor of
 * the class (the string of the MAC). 0 on success. */
int port_x86_64_xhci_control(struct port_x86_64_usb_device *device, k_u8_t request_type,
                             k_u8_t request, k_u16_t value, k_u16_t index, k_u16_t length,
                             void *data);

/*
 * One bulk transfer of `length` bytes from or to the buffer of the caller,
 * which may be a page of a program: the controller gets a physical address
 * per page, as ADR 0032 says for every device. The answer is 0 on success,
 * a negative code on a device that stalled or did not answer in `timeout_ms`,
 * and `moved` is the bytes that crossed.
 */
int port_x86_64_xhci_bulk(struct port_x86_64_usb_device *device, bool in, void *buffer,
                          k_u32_t length, k_u32_t timeout_ms, k_u32_t *moved);

/*
 * One interrupt IN transfer, for an endpoint the class driver turned on with
 * `port_x86_64_xhci_endpoint(..., false, ...)`. Same contract as bulk: 0 on
 * success, bytes moved in `moved`, negative on a stall or a timeout.
 */
int port_x86_64_xhci_interrupt(struct port_x86_64_usb_device *device, void *buffer, k_u32_t length,
                               k_u32_t timeout_ms, k_u32_t *moved);

#endif /* MICRO_OS_PORT_X86_64_XHCI_H */
