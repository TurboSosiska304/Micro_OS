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

### 2. The line of the controller: polling first, then MSI/MSI-X

The first iteration of M1 left `USBCMD.INTE` off and read the event ring from
the tasks that ask for a key: `k_port_console_poll()` and the reader of
`/dev/input0` call the pump of the driver, which takes the events that are
there, decodes the reports and puts the next report of the keyboard in its
ring. That was enough for one keyboard of eight bytes at ten milliseconds and
it cost nothing on a machine of the release; it is not the design of the
second iteration, because an idle core that is woken by a poll of the console
has the latency of the poll and a disk of a gigabyte per second is not a
keyboard.

The controller now owns a line. The order of the price is **MSI**, then
**MSI-X**, then the **INTx** pin of the PCI function, then the poll:

* MSI is one vector, one message and no shared line: the capability at the
  list of the configuration space names the address of the local APIC and the
  data word is the vector (`0x60`, above the timer and the IPI and below the
  door of the programs). The controller of QEMU of the test exposes MSI-X and
  not MSI, so the second form is the one the gate exercises: a table in the
  window of the function (base address register 0), the entry of vector 0
  written with the same address and vector, the enable bit of the capability
  set.
* The INTx pin is the fallback of a function without a message capability and
  it is taken only on a machine that kept the 8259. A machine whose timer and
  keyboard are on the I/O APIC has no redirection entry for a PCI line that
  the firmware table describes (it names the ISA lines and their polarity),
  and a PCI line is level and active low: the driver keeps the poll rather
  than an interrupt with the wrong trigger, and says so in the boot log.
* The handler does not read the event ring. It clears the pending bit of the
  interrupter (RW1C), writes the busy bit of the event ring only when the
  ring is empty, counts the interrupt and wakes the console. The ring is read
  by the pump in the task that asked, because this driver sends one command
  at a time and waits for its completion in that task: a second reader in the
  handler would take an event from under the wait. The first clear of the
  busy bit was the defect of the lane - writing the dequeue behind an event
  the pump had not read yet asks QEMU for another interrupt at once, and the
  machine spends the boot in the handler (one event, a storm of messages).

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

The pool of the driver is eight slots and eight device contexts: a machine of
the release has a keyboard, a mouse, a stick, maybe a wire, and the hubs that
carry them. The context of a device is 2 KiB and the pool is 16 KiB of the
image - the price of a fixed size that no allocator has to be asked for at
the boot. A slot is not its place in the pool: a hub arrives before the
devices behind it and a device that leaves gives its number back, so the pool
is searched by the number the controller gave.

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
* What the line does **not** have yet: RNDIS (the tethering of Android), the
  notify endpoint of CDC-ECM, a hub of the SuperSpeed whose ports need the
  layout of the USB 3.0 status and a transaction translator, and the block
  device of a stick that is unplugged while a volume of it is mounted. They
  are the rows of `docs/debts.md` and the next lanes of 1.8.
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

### 8. What the line still did not have

* **the hub** (class 09): the ports of a controller are enumerated and the
  ports below a hub are not; a laptop whose keyboard is behind the hub of a
  dock is a row of `docs/debts.md` - **closed by §9 of the second
  iteration**;
* **RNDIS** and the telephone of Android;
* **the write of the stick**: `WRITE(10)` is there and the door of the rule
  is the `mount -w` of M2;
* **the interrupt of the controller**: the event ring is polled, which is
  enough for a keyboard and a wire of a few frames and is not the design of a
  disk of a gigabyte per second - **closed by §12 of the second iteration**.

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

## The second iteration: the hub, the hot-plug, the mouse and the line

### 9. The hub of the class 09, and the route string

A dock of a laptop is a hub, and the keyboard, the stick and the wire of the
person are behind it. The hub is a device of the class 09 whose configuration
has one interface and one interrupt endpoint; the driver reads its descriptor
(bNbrPorts, wHubCharacteristics, bPwrOn2PwrGood), powers every port
(`SET_PORT_FEATURE(PORT_POWER)`), waits the time the hub asks for, and walks
the ports with `GET_PORT_STATUS`. A port with a device is reset with the class
request `PORT_RESET`, the change bit `C_PORT_RESET` is cleared, and the speed
is read out of the status of the port.

The devices behind the hub are addressed with a **route string**: four bits
per tier, the port numbers of the hubs above the device, the first nibble for
the hub that is directly on the root port. The slot context carries the route
and the number of the root port, and `Address Device` finds the device the
same way the controller does. The depth is four tiers and the width is eight
ports, which is the ceiling of this driver and more than a dock has.

The hub is told that it is one (`HUB` bit and the number of ports of the slot
context, through `Evaluate Context`) and its descriptor gives the count. A
hub of the SuperSpeed is recognized by the protocol of its device descriptor;
its ports are read with the same four-byte status, and the speed of a device
on one is SuperSpeed. The transaction translator of a high-speed hub with a
full-speed device behind it is not programmed: the device of QEMU is a hub of
the full speed, so the gate exercises the path that needs no split
transaction, and the row of the debt names the rest.

The ports of a hub are polled, not carried by its interrupt endpoint: the
status of eight ports is eight control transfers, which is why the scan is
four times a second and the root ports (a register read each) at every pump.
The interrupt endpoint of the hub is a second consumer of the event ring for
a change at the pace of a hand, and this release spends neither.

### 10. The hot-plug of the ports

`PORTSC.CSC` of a root port is the connect change of a laptop, and the
`C_PORT_CONNECTION`, `C_PORT_ENABLE` and `C_PORT_RESET` bits of a hub port
are the same news below it. The pump of the driver scans the root ports on
every call of the console and the ports of the hubs four times a second; a
port that changed is cleared, a port with a device is reset and enumerated,
and a port whose connection is gone loses its devices: the keyboard and the
mouse forget their slot, the stick leaves the block layer with its
partitions, the wire answers "no link", and the slot goes back to the
controller (`Disable Slot`).

The scan issues commands of the controller, so it runs outside the critical
section of the pump and behind the lock of the driver; a second core that
finds the flag of the scan set leaves it to the one that has it. The devices
of one hub port are found by the prefix of their route, and a hub that leaves
takes the devices behind it with it.

### 11. The mouse of the boot protocol

A mouse of the boot protocol (HID 1.11, B.2) is three bytes - the buttons,
the signed difference of the x axis and the signed difference of the y axis -
and a mouse with a wheel adds a fourth. Its interface is the class 3,
sub-class 1, protocol 2, and its endpoint is an interrupt IN as the
keyboard's is. The decoder is portable (`kernel/input_hid.c`,
`k_input_hid_mouse_decode`): it turns the differences into the pointer events
of `/dev/input0` - `K_INPUT_KEY_POINTER_X`, `K_INPUT_KEY_POINTER_Y`, the
button on its edge and `K_INPUT_KEY_POINTER_WHEEL`, the last number of the
contract - and the y axis is turned over for the screen. A program reads one
device and does not know whether the pointer is the PS/2 one or the USB one.

### 12. The line, and what it costs

MSI-X is the form of QEMU's controller and the one the gate reads:
`xhci: the line of the controller is MSI-X, vector 96` and, after the first
event, `xhci: the event ring raised the line 96 (N interrupt(s))`. The
handler is a stub of `interrupts.S` and a body of `src/xhci.c`; the vector is
0x60, the enable is `USBCMD.INTE` and `IMAN.IE`, and the end of it is the
local APIC of the core 0 (`port_x86_64_apic_eoi()`), because a message of MSI
is an edge and a PCI function of a machine whose lines are on the I/O APIC
cannot be given a level entry from the firmware table.

The gate of the second iteration is `make usb-smoke` with four more machines
besides the three classes of the first half: a keyboard behind a hub (the
route string, the port power and the status), a keyboard added and taken away
on a live machine (`device_add` and `device_del` of the monitor), a mouse
behind the hub whose movements and button `/bin/keys` reads, and the boot line
of the controller with the first event it raised. QEMU names a port of a USB
bus by the path above it (`bus=xhci.0,port=1.1` is the port 1 of the hub that
is on the root port 1), not by the identifier of the hub (`hub.0` of older
documentation), and the tool uses the name the machine of the release has.

### 13. What the second iteration does not have

* RNDIS and the notify endpoint of CDC (unchanged from §7 and §8);
* a high-speed hub with a full-speed device behind it: the fields of the
  transaction translator of the slot context are zero (`docs/debts.md`);
* the ports of a hub of the SuperSpeed read with the layout of the USB 3.0
  status and its link states: the device of the gate is a hub of the full
  speed and a hub of the USB 3.0 is a row of the debts;
* a stick unplugged while a volume of it is mounted: the driver gives the
  block device back and the reads of the mount fail, but the VFS has no call
  that says which volume a point holds, so the table of the mounts is not
  told anything and the volume is not unmounted;
* the second `/dev/input` of a pointer of its own: the contract of ADR 0028
  is one device of the person, and the mouse of the USB is the same one.

