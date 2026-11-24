# ADR 0040 - The keyboard of the USB: xHCI, the rings and the HID of the boot protocol

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0028 (a key is an event), ADR 0032 (the address of a buffer a
  device reads), ADR 0012 (many cores), `docs/plan-1.8.md` (M1),
  `docs/desktop.md`, `docs/ports/x86_64/README.md`

## Context

The release of 1.7 boots on a laptop from a stick, and the laptop has no PS/2
port its firmware will share: the keyboard of a machine of these years is a
device of the USB, and the only way to it is the host controller of the PCI
bus. A machine of that kind has no COM port either, so a kernel without a
driver of the USB is a kernel whose screen says "the kernel is up" and whose
shell nobody can type at. That is the first half of the first lane of 1.8
(`docs/plan-1.8.md`, M1) and this ADR records the decisions of it: the
controller, the memory it reads, the class of the keyboard and the two ways
the keys leave the driver.

Three buses of the USB exist in the world of the PC and one of them is on
every machine made since 2012: xHCI, the host controller of USB 3.0, whose
programming interface is a set of rings of TRBs in the memory of the machine
and not a set of registers to poke per transfer. The older UHCI/OHCI/EHCI
controllers are the machine of a desktop of 2008; they are a second driver
with a second model of the world, and this release is the laptop.

## Decision

### 1. One driver, the rings, and the physical address of ADR 0032

`ports/port_x86_64/src/xhci.c` finds the function of the PCI bus (class
0x0C, sub-class 0x03, programming interface 0x30), maps its memory window
with `port_x86_64_paging_map_mmio()` (uncached, at 2 MiB pages of the high
half), stops and resets the controller, and programs the four structures the
specification names:

* the **capability** registers, which say the length of the register set, the
  offset of the doorbells and of the runtime registers, and how many ports
  and slots the controller has;
* the **command ring**: an array of TRBs whose last entry is a link back to
  the first, with the cycle bit of the producer turning over at the link. A
  command is one TRB and a doorbell write; the answer is a Command Completion
  Event that names the address of the TRB that asked;
* the **event ring** and its **ERST**, one segment of 64 TRBs. The interrupter
  is at `RTSOFF + 0x20` - the first trap of this driver, and the one that cost
  the evening: reading and writing the interrupter at `RTSOFF` makes QEMU
  answer "unimplemented runtime write" and the ring stays empty;
* the **device context base address array** (DCBAAAP) and one device context
  per slot, with the input context of every command (the slot context, the
  context of the endpoint 0 and of the endpoint of the reports).

Everything the controller reads is a **physical** address: the rings, the
contexts, the buffers of the reports and the descriptor of the enumeration.
On this port the image of the kernel lives in the top 2 GiB and the heap in
the identity map, so the translation is the one ADR 0032 wrote -
`port_x86_64_bus_phys()` - and the driver has no second opinion about an
address. The memory is in the image of the kernel and aligned to the 64 bytes
the specification asks; nothing of it is allocated at the boot, so a
controller that is not there costs not one byte.

### 2. Polling, and no interrupt of the controller

`USBCMD.INTE` is off and the event ring is read by the tasks that ask for a
key: `k_port_console_poll()` and the reader of `/dev/input0` call the pump of
the driver, which takes the events that are there, decodes the reports and
puts the next report of the keyboard in its ring. The port has no line per
device yet (`docs/debts.md`, the interrupt a device cannot own), and a
keyboard of eight bytes at ten milliseconds is not a device that needs one.
The alternative - the legacy INTx line of the PCI function through the I/O
APIC, or MSI-X - is a change of the port and not of this class, and it waits
for the lane of the interrupts.

A real controller needs one thing the polling does not give it for free: the
report ring of the endpoint must never be empty, or the device NAKs its
person's key until the next poll. The driver arms **seven** normal TRBs of
eight bytes at the configure, and every completion re-arms one, so the device
has seven places to put a report between two polls. The typematic repeat of a
boot keyboard is a report equal to the one before it, and the decoder of the
kernel turns it into the characters of the key and no edge (see §4).

The timeouts of the driver are the clock of the machine (`k_port_uptime_ms`,
the PIT) with a count of iterations as the guard of a machine whose clock is
not there: a port whose controller never answers is a keyboard that does not
come up, not a machine that hangs.

### 3. The enumeration of one device, and the class of the keyboard

The ports are reset one at a time. A port with a device is read for its speed
and its number, a slot is enabled, the device is addressed (`Address Device`
with the context of the endpoint 0, whose largest packet is the one of the
speed until the descriptor says its own), and the descriptor of the device
gives the largest packet of the endpoint 0 of a full-speed device that is not
8 (an `Evaluate Context` puts it right before the rest of the descriptor
arrives). The configuration descriptor is read twice - nine bytes for its
length and the whole of it - and parsed for the interface of class 3 (HID),
sub-class 1, protocol 1 (the boot protocol of a keyboard) and its interrupt
endpoint of the device to the host. A device that is not that one is
enumerated to the point of its configuration and gives its slot back.

The keyboard is then configured (`SET_CONFIGURATION`), told to speak the boot
protocol (`SET_PROTOCOL`, 0), and its endpoint 1 IN is set up with
`Configure Endpoint`: the type 7 of the context, the largest packet of the
descriptor, the interval of it and the ring of the reports.

The **boot protocol** is the decision and not the report protocol of the HID:
a report of eight bytes - the mask of the eight modifiers, one reserved byte
and six usages of the keyboard page - is the same on every keyboard of the
world, it does not need the report descriptor of the device, and the
difference between two reports is the press and the release of a key. The
report protocol would need a parser of the descriptor of every device, which
is a program of drivers and not a driver of a keyboard.

The pool of the driver is four slots and four device contexts: a machine of
the release has a keyboard, maybe a mouse, and the ports of a hub that this
lane does not enumerate are not devices of it. The context of a device is
2 KiB and the pool is 8 KiB of the image - the price of a fixed size that no
allocator has to be asked for at the boot.

### 4. The report is decoded by the portable kernel, not by the port

`components/kernel/src/input_hid.c` is the table of the usages, the state of
the modifiers, Caps Lock and the six slots, and the difference between one
report and the next. It is C11 with no hardware in it, the sibling of
`input_set1.c`, and it answers the events of the contract
(`struct k_input_hid_event`: a key of the machine, an edge, the character it
types, and whether the edge is the typematic echo of a held key). The port
pushes those events into the same two rings the PS/2 fills - the characters
of the console and the events of `/dev/input0` - and the two keyboards of the
machine are therefore one keyboard to the shell and to a program. The host
test `tests/cases/test_input_hid.c` drives the decoder with the reports of
the specification: a press and a release, Shift and Caps Lock, Ctrl+C, six
keys at once, a report that repeats, a usage that is unknown and a report
that is cut by the cap of the caller.

The usage is not the scancode of the PC (0x04 is `a`, 0x1E is `1`) and the
number of a key of the machine is neither, so the table is the translation of
a keyboard page and not a second set 1. The characters are the US layout of
the PS/2 map, for the same reason: there is no `keymap` in this release and a
second layout would be a table of the port with no test behind it.

### 5. The machine without a controller, and the two keyboards

`port_x86_64_xhci_init()` is called after the table of the interrupts exists
(the waits of the enumeration are measured by the clock) and it is optional
in the strongest sense: a machine whose PCI bus has no xHCI function boots
exactly as it did before, the PS/2 keeps its keys, and the only trace of the
call is a line of the debug level. A machine with both keyboards gives the
console the PS/2 first and the USB after it, and `/dev/input0` merges the two
rings; the capture of the keyboard by an interface
(`port_x86_64_input_capture`) turns the characters of both off and empties
both rings, so a `q` of an interface is not a command of the shell that waits
for it.

## Alternatives

**A. UHCI (or OHCI, or EHCI) first.** The older controllers are simpler per
transfer and unusable on the machine of the release: a laptop of these years
has the xHCI of the chipset and no companion controller, so a driver of UHCI
would be a driver of a desktop of 2008 and the second driver would still be
written. The rings of xHCI are also the shape the next drivers of the port
(AHCI, NVMe) will reuse.

**B. The interrupt of the controller now.** MSI-X or the legacy INTx line
gives the keys at the moment they arrive instead of the next poll. It needs
the port to own a vector per device, which it does not; the poll is at the
rate of the shell and the endpoint carries seven reports, so the latency is
the one of a person and not the one of the machine. It is a row of
`docs/debts.md` and a lane of the port.

**C. The report protocol of the HID.** A parser of report descriptors would
read a keyboard of any shape, a consumer-control device included. It is the
right answer for a system of many devices; it is not the answer for the
keyboard of a laptop, and the boot protocol is the one the BIOS and every
keyboard agree on.

**D. The USB keyboard as a second `/dev/input1`.** Two devices would make a
program read two files for one person and a desktop merge two streams; the
contract of ADR 0028 is one device of the keys of the machine, and the two
keyboards of a laptop are two ways to the same keys.

**E. A driver of the class in a program of ring 3.** The controller is a bus
master of the memory of the kernel, and the rings of a program are not the
addresses the controller reads; a userland driver would need the physical
address of its buffers through a call the ABI does not have. The driver is a
driver of the port.

## Consequences

* A laptop with no PS/2 port boots to a shell a person can type at, and the
  gate is `make x86-usb-smoke`: the machine of the test has no i8042 at all
  (`-machine pc,i8042=off`) and one `usb-kbd` on a `qemu-xhci`, and the test
  types a line of the shell through the monitor of QEMU (`sendkey`), reads
  the answer, and then reads the keys themselves out of `/dev/input0` through
  `/bin/keys` - the code of `a` and its press and release, and the code of
  Enter. The transcript of that run cannot be a keystroke of a PS/2, because
  the machine has none.
* The boot log names the controller and the device: `xhci: controller at
  febf0000, version 1.0, 8 port(s), 64 slot(s)` and `usb: keyboard 0627:0001
  on port 5, slot 1, interface 0, 8 bytes`.
* The keyboard of the board is unchanged and the host tests do not need a
  controller: the decoder of the report is portable and tested with reports,
  and the driver of the controller is the port's.
* What the line does **not** have yet: the HID mouse of the USB (the pointer
  of the release is the PS/2 one), mass storage (the stick of M1's second
  half), CDC-ECM/RNDIS, a hub with the ports below it, and the interrupt of
  the controller. They are the rows of `docs/debts.md` and the next lanes of
  1.8.
* A device of another class on a port is enumerated and refused, and the
  controller is left with the slots of the devices it understood; a stick of
  the second half plugs into the same enumeration and takes the next step
  from there.
