# ADR 0040 - The USB of a laptop: the controller, the keyboard, the stick and the wire

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0028 (a key is an event), ADR 0032 (the address of a buffer a
  device reads), ADR 0012 (many cores), ADR 0041 (a disk nobody formatted),
  ADR 0008 (block devices), `docs/plan-1.8.md` (M1), `docs/desktop.md`,
  `docs/ports/x86_64/README.md`

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
  controller is left with the slots of the devices it understood.

## The second half of M1: the stick and the wire

### 6. The stick: the Bulk-Only Transport on the block layer

A stick is a SCSI disk behind one interface: the class 08, sub-class 06 and
protocol 0x50 is the **Bulk-Only Transport**, and it moves the commands of
that disk over two bulk endpoints. One command is three transfers - a Command
Block Wrapper of 31 bytes ("USBC", a tag, the length and the CDB), the data of
the command in the direction the CDB says, and a Command Status Wrapper of 13
bytes ("USBS", the same tag, a residue and a status) - and the signatures and
the tag are the proof that the three are one command.
`src/usb_storage.c` is that class: `TEST UNIT READY`, `INQUIRY`,
`READ CAPACITY(10)`, `READ(10)` and `WRITE(10)`, and a `struct k_blockdev`
registered as `usb0`.

The unit of the block layer is 1 KiB, as on the two other disks of the port,
so one sector of the filesystem is two 512-byte sectors of the medium and the
arithmetic lives in the driver. A command moves at most 32 KiB - eight pieces
of a page, eight TRBs of the ring of the endpoint - and a read of the block
layer is cut into commands of that size. The buffer of the caller may be a
page of a program, so every piece of a page is translated with
`port_x86_64_paging_phys()` and the controller gets a physical address per
piece (ADR 0032 §1), exactly as the AHCI driver does.

The medium is the one of a person, and the rule of **ADR 0041** is the first
thing the attach does: the whole disk and its first partition are identified
by `kernel/volume.c`, the disk is registered read-only with a write-through
cache, and a write is refused with the rule's sentence (a volume this system
did not make, a volume it did not name) or with the one of the table (a
sector below the first partition of the medium). `WRITE(10)` is implemented
behind the same closed door the AHCI driver keeps, opened by the `mount -w` of
M2 and not by a mount.

`usb0` is a disk of the port beside the IDE channel, virtio and AHCI: the
block layer, `partition_scan()` and the rule do not know which controller is
under them, and the boot's root chain prefers an internal disk to a stick in
a port. A machine with only a stick boots from it.

One limit of the tree is in the way of a stick of MicroFS: `mfs_mount()` holds
one volume at a time, so mounting a MicroFS stick beside a MicroFS root
replaces the root's state. The gate uses an ext2 volume on the stick for that
reason (a volume of another system, which is also the stronger case of the
rule), and the single-volume MicroFS is a row of `docs/debts.md`.

### 7. The wire: CDC-ECM, and not RNDIS

A telephone that shares its connection and a dongle of ethernet appear as one
of two classes: **CDC-ECM**, the standard of the USB, which is ethernet
frames over two bulk endpoints; and **RNDIS**, the same idea in a protocol of
Microsoft with a control channel of its own. This release speaks the first:
the control interface of class 2, sub-class 6, its functional descriptor of
the ethernet (the MAC is the string it names), the data interface of class 10
whose alternate setting 1 carries the two bulk endpoints, and then one class
request - `SET_ETHERNET_PACKET_FILTER` - and one `SET_INTERFACE` for the data
setting. `src/usb_net.c` is that class.

RNDIS is not spoken. It is a message protocol per control transfer
(INITIALIZE, SET, QUERY, KEEPALIVE, an OID for every property of the card),
it is larger than the whole data path of ECM, and the device of QEMU that a
test can give the machine carries ECM in its second configuration - the
enumeration reads every configuration of a device until one names a class of
this release, which is the change the wire forced on the controller's driver.
A telephone whose tethering is RNDIS only (the Android of the default) is a
row of `docs/debts.md`; the firmware of a gadget that offers ECM, a Linux and
an iPhone are the standard.

The notification endpoint of the control interface is **not** configured: it
carries the speed of the link and the notifications of the device, and this
stack has one speed and a link that is up when the device is there. Polling a
second endpoint for a change nothing acts on is a cost of every frame.

The wire is a device under the stack of `src/net.c` and not a second stack:
`src/netdev.c` is the seam (a name, a MAC, the link, a frame in and a frame
out), the card of virtio and the wire of the USB register their operations
there, and the first registration wins - the card of the PCI bus is probed
before the controller of the USB, so a laptop with both has its wire and a
laptop with only the dongle has the dongle. The frames are polled: a receive
asks the bulk endpoint with a timeout of two milliseconds and a device that
has nothing answers "no frame", which is the shape the stack already had.

### 8. What the line still does not have

* **the hub** (class 09): the ports of a controller are enumerated and the
  ports below a hub are not; a laptop whose keyboard is behind the hub of a
  dock is a row of `docs/debts.md`;
* **RNDIS** and the telephone of Android;
* **the write of the stick**: `WRITE(10)` is there and the door of the rule
  is the `mount -w` of M2;
* **the interrupt of the controller**: the event ring is polled, which is
  enough for a keyboard and a wire of a few frames and is not the design of a
  disk of a gigabyte per second.

## Consequences of the second half

* The gate is `make usb-smoke` and it drives the three classes on one machine
  with no PS/2: the keyboard (16 checks of the first half, unchanged), the
  stick (its partition mounted, a file read through the USB, a write refused,
  the SHA-256 of the whole image the same before and after) and the wire (the
  boot names `usbnet0`, `netinfo` reads its MAC, `ping 10.0.2.2` crosses the
  bulk endpoints, the counters move). It is 35 checks and the wire is the
  image of `MICRO_OS_X86_NET=ON`.
* `make usb-stick-smoke USB_STICK=/dev/sdc` is the stick of a person, attached
  read-only: the test mounts its first partition, lists it and compares the
  SHA-256 of the whole device. The device belongs to the host and the run
  needs the permission to read it.
* The boot of a machine with a stick says what it found and what the rule
  answered: `usb0: QEMU QEMU HARDDISK 2.5+, 16 MB on the port 2 of the
  controller`, `usb0: usb0p1: the volume of another system (ext2),
  read-only`, `usb0: the disk is read-only: the medium carries a volume of
  another system`, and `usb: 1 disk(s) of the storage class, read-only,
  nothing written`.
* The network of the port is two drivers behind one seam, and
  `make x86-net-smoke` (virtio) and `make usb-smoke` (the wire) are the same
  stack on both.
