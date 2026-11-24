/*
 * Micro OS — the USB 3.0 controller of the x86-64 port (ADR 0040).
 *
 * The machine of a laptop of these years has no PS/2 port the firmware will
 * share and no COM port, so the keyboard of the person in front of the screen
 * arrives through the host controller of the USB (xHCI, PCI class 0x0C,
 * sub-class 0x03, programming interface 0x30). This header is what the rest
 * of the port knows about it: a boot call, the characters of the console, the
 * events of `/dev/input0` and the capture of the keyboard by an interface.
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

/* The next character of the USB keyboard, or -1 when its ring is empty. The
 * call also reads the controller, so a caller that polls the console often
 * is also the caller that moves the reports of the keyboard. */
int port_x86_64_usb_poll(void);

/* The next event of the USB keyboard, or false when its ring is empty. */
bool port_x86_64_usb_event(struct input_event *out);

/* The keyboard belongs to an interface while it is open: the characters of
 * the console stop and only the events are pushed, as the PS/2 does. */
void port_x86_64_usb_capture(bool on);

#endif /* MICRO_OS_PORT_X86_64_XHCI_H */
