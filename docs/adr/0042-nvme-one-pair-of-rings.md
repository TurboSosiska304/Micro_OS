# ADR 0042 - The disk of a modern laptop: NVMe, one pair of rings

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0008 (block devices), ADR 0032 (the address of a buffer a
  device reads), ADR 0037 (the second disk), ADR 0041 (a disk this system did
  not format is read-only), `docs/plan-1.8.md` (M2), `docs/debts.md` (K14)

## Context

The SATA controller of [ADR 0041](0041-a-disk-nobody-formatted.md) gave the
port the disk of the transition: a chipset function with one command list per
port, the FIS of the drive and a table of physical addresses. It also gave the
port the rule that matters more than either driver - a disk this system did not
format is read-only - and the proof of it by a hash.

A laptop of these years does not have that controller. The disk is behind
**NVMe**: a function of the PCI bus (class 01, sub-class 08) with a 64-bit
window in BAR0, no channels and no task file, and a model of the host and the
device that is the opposite of the ATA one. The device is a set of **queues**;
each queue is two rings of memory the host owns - a submission queue the host
fills and a completion queue the device fills - and a doorbell for each ring.
A command is 64 bytes and a completion is 16, the host and the device never
share a register of state, and the number of commands in flight is the depth
of the ring and not one.

The plan of 1.8 puts that controller in the same lane as the SATA one
(`docs/plan-1.8.md`, M2): the second half is the queue pair, the identify data
and the read of the volume, under the rule that the first half wrote down.

## Decision

### 1. The controller is a window, a state machine and two queues

The registers are one window the kernel maps uncached into the top 2 GiB
(`port_x86_64_paging_map_mmio`, above the screen, the APIC, the USB and SATA
windows), and the driver reads them as numbers: `CAP` for the depth of a queue
and the stride of the doorbells, `VS` for the version, `CC`/`CSTS` for the
enable and the ready bit, `AQA`/`ASQ`/`ACQ` for the admin queues. The controller
comes out of a reset disabled; the boot programs the sizes and the two
addresses, sets `CC.EN` with the two entry sizes (64 bytes of a command, 16 of
a completion) and the page size of the machine, and waits for `CSTS.RDY` with a
budget of five seconds. A function that does not become ready is a line in the
log and no disk, not a machine that waits forever.

### 2. The boot speaks three commands of the admin queue and one of the NVM set

`Identify Controller` says who the drive is (the model and the serial, for the
log, and how many namespaces it has) and `Identify Namespace` says how the
namespace is formatted - the number of logical blocks and the size of one,
taken from the LBA format the namespace selects. The namespace of this release
is a run of **512-byte blocks**, which is the sector of the medium as the
kernel and the two other disks call it; a namespace of another size is refused
with the number it wants and the number this system has, because the block of
MicroFS is a kilobyte and the driver has no view of a grain above the medium
(the lane of M4 of the plan).

`Create I/O Completion Queue` and `Create I/O Submission Queue` make **one**
pair, the pair 1, both physically contiguous and both polled: the completion
queue is created without an interrupt vector and without `IEN`, and the driver
reads the completions the device writes. The submission queue is 64 entries,
clamped to what `CAP.MQES` allows. `READ` (opcode 0x02) is the one command the
NVM set carries in this file.

The rings live in the image of the kernel, and every address the device is
given is a physical one: `port_x86_64_dma_phys()` is the shared translation of
ADR 0032 (`port_x86_64_bus_phys` for the heap and the image, the page tables of
the moment for the window of a program), and the two drivers of the lane use
it.

### 3. PRP1 and PRP2, the two pages of a request

The bytes of a read are named by two physical addresses: `PRP1` is the page of
the buffer with the offset of the buffer in it, and `PRP2` is the page after
the first when the request leaves it. A request therefore names at most two
pages - 8 KiB from an aligned buffer - and the driver splits a larger run of
the block layer into as many commands as it needs. The PRP **list** (the page
of addresses that names more than two) is not written in this step: it is the
next thing a read of a big file wants, and it is a row of `docs/debts.md`, not
a missing piece of the contract - the contract is "the pages of the caller,
one address per page", and two of them are enough for the volume of this
machine.

A buffer that starts in the middle of a page is the case the specification
calls the offset of `PRP1`, and it is the case a filesystem meets all the time:
the block layer hands the driver the address of a member of a structure. The
translation of the port keeps the offset of a page, so the arithmetic of the
pieces is the same before and after it.

### 4. One command at a time, and the completion is polled

The driver keeps one command in flight: the lock of the driver is taken for a
run of the block layer (a read ahead of a file is one run and several
commands), the ring is rung once per command and the completion is read from
the head of the completion queue, where the phase bit says whether the device
has written it. The interrupt of the controller is **not** opened: MSI-X is a
table of vectors and a capability of the function, and this step of the lane
does not have one. A poll with a budget of five seconds is the same shape as
the poll of the SATA port and of the IDE channel; the line is a debt, not a
decision.

The alternative - a queue depth of 64 and many commands in flight - is the
reason the rings are 64 entries deep, and it is the next step after the
interrupt: neither a boot nor a filesystem of this release needs more than one
command of the medium at a time, and a driver that pipelines without a
completion line is a driver that polls two rings instead of one.

### 5. The disk is read-only, by the rule of ADR 0041

Nothing of the rule is here: the boot asks `k_volume_identify()` what the
medium carries, `k_volume_may_write()` whether it may be written (the header
block of MicroFS **and** a name in `/etc/volumes`), and keeps the boundary of
the table of the partitions for the writes that never come. **There is no
WRITE command in this file.** The write of the block layer is answered with
the sentence of the rule - once in the log of the boot - and the error of the
medium to the caller; the cache of the block layer above the disk is
write-through, so the refusal is the answer of the call and not of a sync
nobody asked for.

The first boot of a machine with the controller lists what it found, in the
same shape as the SATA one:

```
nvme0: CAP 0xf0107ff: 2048 entries of a queue at most, a stride of 4, CSS 0x41
nvme0: QEMU NVMe Ctrl microos, 256 namespace(s), NVMe 1.4.0
nvme0: 16 MB, 512-byte blocks, 64 entries in the pair of the I/O queues
nvme0: nvme0p1: a volume of this system (mfs), read-only
nvme0: the disk is read-only: the volume is not named in /etc/volumes
x86-64: the root volume is nvme0p1 (1 partition(s))
nvme: 1 disk(s) on the controller, read-only, nothing written
```

### 6. The proof is the same hash

`make nvme-smoke` boots the image of `make x86-disk` on the NVMe namespace of a
QEMU machine, mounts the volume of `nvme0p1` as the root, lists `/bin`, reads
`/etc/setting`, runs `/bin/hello` out of a file of the NVMe disk, tries a write
and gets the refusal, and compares the SHA-256 of the whole image before and
after the boot. The second boot replaces the volume with one of btrfs: the
mount is refused with the name of the foreign system, the machine stops, and
the hash of that image is the same too.

## Alternatives

**A PRP list from the start.** One page of addresses names two megabytes, and
a read ahead of the block layer (128 KiB) would be one command. It is the
right shape for a file and the wrong shape for a first step: the two-page case
is what the specification calls the normal case of a small request, every
filesystem of this machine reads a kilobyte or a few at a time, and a list is
another page of memory, another translation of it and another error to get
wrong. The step is written down (`docs/debts.md`) with the size at which it
pays.

**MSI-X and the interrupt first.** A completion queue with an interrupt vector
is the interface of a driver of a real kernel: the device raises a message and
the vector runs the completion. That needs the capability of the function, a
table of vectors in BAR0, the LAPIC/IOAPIC routing and a handler; the polling
loop is twenty lines and the boot of this machine is a handful of reads. The
line is a debt of the lane and not a reason to hold the disk.

**Many queues, a queue per core.** `CAP.MQES` and the many queue pairs are how
a modern disk is kept busy by a machine with many cores. This kernel has a
scheduler of one queue per core (ADR 0012) and a block layer with one caller
at a time behind a lock; a queue per core would be a change of the block
layer, not of the driver. One pair is the shape of the release.

## Consequences

* The disk of a laptop is readable in the release that has NVMe: the volume of
  the first partition is the root of a machine with no IDE channel and no
  SATA port, and `/bin` runs out of it.
* The write path stays closed exactly as on the SATA disk: the two drivers of
  the lane answer with the same sentence, and the day `mount -w` lands both of
  them open with the same rule.
* The driver is one queue pair and one command in flight: a read of a small
  file is one command, a run of the block layer is one command for every 8 KiB,
  and the number is a debt with the PRP list, not a promise of the release.
* `port_x86_64_dma_phys()` is the shared translation of ADR 0032, and the SATA
  driver uses it too; a third bus master of the port has one call to learn.
* A namespace of 4096-byte blocks, a controller whose smallest page is not
  4 KiB, and a function behind a PCI bridge (the walk of the port stops at bus
  0) are refused or invisible with a sentence, not misread.

## What this ADR does not do

* it does not write: no `WRITE` command, no flush, no `mount -w`; the writable
  root and `pkg` for good are the last step of M2;
* it does not open MSI-X, the interrupt of a completion queue, or more than one
  queue pair;
* it does not name more than two pages of a buffer: the PRP list is the next
  step of the read path and the number that pays for it is written down;
* it does not follow a controller behind a PCI-to-PCI bridge, allocate more
  than one namespace, or react to a medium that appears after the boot;
* it does not format, repair or claim a disk: the rule of ADR 0041 is the only
  answer to a medium this system did not make.
