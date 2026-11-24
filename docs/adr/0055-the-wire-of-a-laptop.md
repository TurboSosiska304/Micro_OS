# ADR 0055 - The wire of a laptop: the e1000e of Intel, and an address nobody typed

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0007 (paging and the mapping of a device), ADR 0012 (many
  cores), ADR 0023 (the screen of x86-64), ADR 0032 (the address of a buffer a
  device reads), ADR 0039 (Wi-Fi, the lane that follows this one), ADR 0040
  (the wire of the USB), ADR 0042 (NVMe, the same window), `docs/plan-2.0.md`
  (M5), `docs/debts.md` (K6)

## Context

K6 gave the port a stack of its own - ARP, IPv4, ICMP, UDP, a client and a
server of TCP, the resolver - and one card under it: virtio-net in its legacy
interface, chosen for a reason of the address map and not of taste. The legacy
virtio card is programmed through port I/O, so the rings are the only memory
the device sees and the kernel's identity map of the first gigabyte is enough;
the MMIO BAR of a PCI card of QEMU sits near 4 GiB, outside that map.

The address of the machine was a constant: `10.0.2.15/24`, the gateway
`10.0.2.2`, the resolver `10.0.2.3` - the user network of QEMU. On a laptop of
a person that is a machine which does not connect: the address of a house is
not a number of the tree, and the plan of 2.0 (M5) asks for the opposite - the
wire of the laptop, an address, a gateway and a resolver nobody typed.

Two wires are in reach for a laptop of these years:

* the **Intel I219** on the PCI bus, the family of the `e1000e` of the driver
  of Linux. The owner's ThinkPad has it, and QEMU models it (`-device e1000e`,
  the 82574L) - so the gate of the work is a machine of the port and not a desk
  with a dongle on it;
* the **RTL8153** on the USB, which 1.8 already has a place for: the port
  speaks the class of CDC-ECM behind xHCI (`src/usb_net.c`, ADR 0040), and a
  network of a laptop without a card is a stick of this shape.

## Decision

### 1. The card is the e1000e, and the reason is the gate

The RTL8153 looks cheaper - the USB is enumerated, the class of CDC-ECM is
written, and the driver of 1.8 is a file of three hundred lines - and it is
not: the RTL8153 is **not** a CDC-ECM device. It is a vendor function that
wants a firmware image before it answers, a table of registers learned from the
driver of another system, and a vendor request for every change of the
medium. A driver of it would be as large as the e1000e one and would have
**no gate**: QEMU's `usb-net` is the CDC-ECM of ADR 0040 and not an RTL, so the
only machine that could run the driver is the laptop of the owner.

The I219 has the gate and the machine. QEMU's `-device e1000e` is the 82574L,
which is the same register file for everything this driver touches, the port
already maps MMIO windows of the PCI bus for the screen, the APIC, the SATA
controller and the NVMe disk, and the smoke test of the network becomes three
machines of one image instead of a driver nobody can boot.

The CDC-ECM driver of 1.8 stays: a machine without the card and with a dongle
of that class has its wire, and the seam of `src/netdev.c` (the first driver
that comes up takes the stack) is unchanged. The two drivers are one stack
with one card, and a machine with both has the card of the laptop, because the
port asks for it first.

### 2. The price is one mapping, and the driver is the plain half of the card

The BAR0 of the card is 128 KiB of memory above the identity map. The driver
maps one 2 MiB page of it into the top 2 GiB of the kernel with
`port_x86_64_paging_map_mmio()` - the same window and the same rule as the
SATA controller (ADR 0041) and the NVMe disk (ADR 0042) - and reads the
registers through it. Every address the device is given is a physical one:
`port_x86_64_bus_phys()` is the translation of ADR 0032, and the rings and
the buffers come from the heap, which is the identity map.

The driver speaks the **legacy** descriptor of the family: sixteen receive
descriptors of one 2 KiB buffer and eight transmit ones, `RFCTL.EXTEN` clear
(the value of the reset), no checksum offload, no advanced descriptor and no
interrupt. The card is polled by the stack the way the virtio card is: the
receive ring is asked for a frame, and the transmit path waits for the done bit
of its descriptor. A polled card costs the stack nothing new, and a line of
the card would need the I/O APIC (K8).

The MAC comes out of the NVM through `EERD`, with the field layout of the
**e1000e** and not of the old 82540: the address at bits 2..15, `DONE` at bit
1. A driver that copies the numbers of the other family waits for a bit this
card never sets - the first version of this file did, and the boot said
"the NVM of the card does not name a MAC" instead of a card. The address is
written into the filter `RA0` with `AV`, so unicast frames of the machine are
taken, and the counters of the driver are the ones `netinfo` prints.

The waits of the init are **counted**, not measured: the init of a device runs
before the timer of the machine (`port_x86_64_net_init()` is called before
`port_x86_64_interrupts_init()`), where `k_port_uptime_ms()` is a zero that
never moves. A deadline of the clock there is a boot that hangs on a card that
is not there; a budget of reads is a line in the log and no card.

### 3. DHCP is portable, and the port lends it the wire

The client is `components/net/src/dhcp.c` and it is portable C11: it writes the
four messages of RFC 2131 - DISCOVER, OFFER, REQUEST, ACK - into a buffer and
hands each one to a transport of four calls (send, receive, the clock, and the
apply of a lease), which is the stack of the x86-64 port on the machine and a
scripted server in `tests/cases/test_dhcp.c`. Nothing of the client knows a
socket, a frame or a port, which is what lets the same file be tested byte by
byte on the host and be used by a machine.

The exchange is the one of a client with no address: a broadcast DISCOVER from
`0.0.0.0:68` to `255.255.255.255:67` with the broadcast flag, so the server's
answer is a broadcast frame and a machine that has no address can receive it.
The transaction id is a mix of the MAC, the clock and a counter. Three
attempts share the budget of three seconds, and a REQUEST names the address it
asks for and the server that offered it.

The lease lives as long as the server says and is renewed at **T1** (half of
it, or option 58) by one REQUEST to the same server, and at **T2** (seven
eighths, or option 59) by a broadcast one - the REBINDING of RFC 2131 §4.3.2,
which is also the shape a renewal of this stack uses, because a client that
cannot be sure its address is routable cannot be reached at it either. A NAK
is a refusal and starts the exchange again; a timeout keeps the address until
T2, because a renewal that did not arrive is not an address lost.

### 4. The lease is a file of the machine, and the fallback is the address of 1.8

`/etc/network` is the file: one `name value` a line, read at the first use of
the network and written after every answer.

```
dhcp 1
address 10.0.2.15
netmask 255.255.255.0
gateway 10.0.2.2
dns 10.0.2.3
source dhcp
server 10.0.2.2
lease 86400
t1 43200
t2 75600
```

Three states, and a person can tell them apart in `netinfo`:

* **leased** - a server answered; the address, the mask, the gateway and the
  resolver are its, the cache of names is cleared, and the file says
  `source dhcp`;
* **fixed by the file** - `dhcp 0` is there, so the client asks nobody and the
  address below is the one of the machine: a house with a server that should
  not be asked is a file of one line;
* **no server** - the link was down, or three DISCOVERs went unanswered in
  three seconds: the machine keeps the fixed address of 1.8 and the log says
  `dhcp: no answer from a server in 3000 ms, the fixed address is in use`. The
  file keeps `dhcp 1` - a fallback is not a decision, and the next boot asks
  again.

The line `net: 10.0.2.15/24, gateway 10.0.2.2, dns 10.0.2.3` is the same line
K6 printed and the smoke test of K6 looks for; the prefix is computed from the
mask now instead of being a `/24` in the format string.

### 5. The gates

* `make test` - the client against the scripted server of
  `tests/cases/test_dhcp.c`: the exchange, the request that names address and
  server, another transaction that is not an answer, a NAK, a timeout of three
  DISCOVERs, a renewal, and the numbers of T1/T2;
* `make x86-net-smoke` - the machine of K6 (virtio) now takes its address from
  the DHCP server of QEMU user-net and keeps it in `/etc/network`, and two more
  machines of the same image run: one with `-device e1000e` (the driver, DHCP,
  the gateway, a file of the host through `wget`, the lease in the file) and
  one on a wire of QEMU's `socket` transport whose other end is a socket of the
  test that never answers (the fallback, the line, and `dhcp 1` kept).

## Consequences

* A machine of the owner with an I219 has a wire. The driver is the plain
  half of the card; what is left for a real one is the PHY and the management
  engine of the chipset, which is a table of the family and not a shape of
  this stack: the line of K6 in `docs/debts.md` names it. The driver forces
  the link up (`CTRL.SLU`) and reads `STATUS.LU`, which is the state of the
  MAC and of the reset; a cable that is pulled is told by the timeout of the
  client of DHCP and not by the card, until the PHY is read.
* The RTL8153 is not driven, and the CDC-ECM stick of 1.8 stays the USB wire.
  A driver of the RTL would have the laptop and no machine of the tree to test
  it on - the same reason this ADR did not take it.
* Wi-Fi is the lane of ADR 0039 and follows this one: a wireless station is a
  stack over a stack, and an address it gets is the same client of DHCP with
  the same file.
* The stack still has one address and one card. A second address, a route
  table and a resolver per interface are the shape of a machine with two
  networks, and this release has one network of its own.
