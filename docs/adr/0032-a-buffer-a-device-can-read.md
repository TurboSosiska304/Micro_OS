# ADR 0032 - The address of a buffer a device reads

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0007 (paging and user mode), ADR 0008 (block devices),
  ADR 0012 (many cores), ADR 0019 (ext2), ADR 0029 (ext2 write),
  `docs/memory.md`, `docs/plan-1.6.md` (M3)

## Context

The disk of the x86-64 port has spoken PIO since ADR 0008: the driver reads
and writes the data register of the channel, one 16-bit word at a time, with
the processor as the mover of every byte. That was the honest first step,
because a bus-master transfer needs something the kernel did not have: the
**physical** address of the buffer.

A processor does not care. It is handed a virtual address, the page tables of
the moment translate it, and the transfer happens. A controller has no page
tables: the PRDT of the IDE channel, the descriptors of virtio, the queues of
NVMe and the registers of every other bus master hold *physical* numbers, and
the device writes to the memory those numbers name. Hand it a virtual address
and it writes somewhere else - into another task, into the page tables, into
the kernel - with no fault and no message. On a machine with an MMU and
without an IOMMU this is the whole of the problem: a wrong translation is not
a slow disk, it is silent corruption of memory the caller never named.

The tree has three kinds of memory, and each of them translates differently on
this port:

| What | Virtual | Physical | Who knows |
|---|---|---|---|
| the image of the kernel | `0xFFFFFFFF80000000` + | `0x01000000` + | `port_x86_64_kernel_phys()` |
| the heap (`k_port_alloc`, `kmm_alloc`) | below 32 MiB | the same number | the identity map of the first GiB |
| a page of a program | `0x02000000`-`0x03000000` | `0x04000000`-`0x05000000` | the page tables of the program (`port_x86_64_paging_phys()`) |

The third row is the one that makes this an ADR and not a comment. Since
ADR 0029 the ext2 writer hands the buffer of the caller down to the block
device: `sys_write` of a program gives `kfile_write` the pointer of the
program, `ext2_write_bytes` splits it into sectors, and `device_write` of the
filesystem calls `dev->ops->write(ctx, sector, src, 1)` with that pointer.
With PIO this works, because the kernel runs on the page tables of the program
during a system call (ADR 0007 §1). With a bus master it would write to
physical `0x02000000` - the middle of the identity map - instead of the frame
behind the window. Nothing in the block layer would notice.

So the contract has to be written down before the driver exists: which
addresses may reach a device, who translates them, and who keeps the memory
alive while the transfer is in flight.

## Decision

### 1. The buffer of a block device is an address of the caller, and the port translates it

`struct k_blockdev_ops` keeps the pointer it has: `read` and `write` take a
buffer of the caller and know nothing about pages, frames or buses. That is
the portable kernel and it stays portable: the board has no MMU, the
simulator has no device, and neither has anything to translate.

The translation is the business of the driver, which is the business of the
port, and it has exactly two answers on x86-64:

```c
k_u64_t port_x86_64_bus_phys(const void *ptr);                    /* kernel memory */
void   *port_x86_64_paging_phys(k_u64_t cr3, k_u64_t vaddr);      /* a program page */
```

The first is arithmetic: below `PORT_X86_64_KERNEL_BASE` the address is the
number (the identity map of the first gigabyte, which holds the heap), above
it the image is `ptr - KERNEL_BASE + KERNEL_PHYS`. The second walks the page
tables of the space that is current now - `port_x86_64_current_cr3()` - and
answers the frame of the entry, or `NULL` when the page is not there. A
driver that is handed an address in `[PORT_X86_64_USER_BASE,
PORT_X86_64_USER_LIMIT)` asks the second one; any other address asks the
first. The window of the programs is 32-48 MiB and the heap stops at 32 MiB,
so the two ranges cannot be confused and no heuristic is needed.

### 2. A transfer is split at the page boundary, and each page is translated on its own

Two adjacent virtual pages are not adjacent frames: the window of a program
maps 4 KiB pages of the arena, the heap is 16-byte blocks inside a region the
identity map covers with 2 MiB pages, and a future mapping of a file (M1) will
be neither. A driver therefore cuts every transfer into pieces that do not
cross a 4 KiB boundary and translates each piece:

* the piece is `min(left, PAGE_SIZE - (vaddr & (PAGE_SIZE - 1)))` bytes;
* its physical address is the translation of the page plus the offset inside
  it;
* the PRDT of the IDE channel gets one entry per piece, and a piece is a whole
  number of sectors (the caller of the driver hands it sectors, so the cut
  falls on a sector boundary of 512 bytes and a page holds eight of them).

A piece that does not translate - a page that is not present, or a frame
above the 4 GiB the PRDT can name - ends the DMA for that transfer. The driver
falls back to PIO for the whole command and says so once in the log. PIO
needs no translation at all, which is the point of keeping it: it is the path
that cannot be wrong about an address, and it is the reason a machine with an
unusual map still has a disk.

### 3. Who owns the memory while the transfer runs

The buffer belongs to the caller and lives for the length of the call. The
driver may not keep the pointer, may not sleep on it, and may not start a
second command that names it; the block layer's mutex and the driver's own
lock give the second half of that promise.

Nothing is pinned, and nothing has to be, for the two reasons the port can
give today:

* there is no swap and no page eviction, so a present page stays where it is
  until the space is destroyed;
* the pages of a program are copied only by the fault handler of a *write of
  the program* (the copy on write of a fork, ADR 0007 §4). A device writes to
  a frame through the bus and never through the tables, so it cannot cause
  that fault, and a program cannot run on the core that is inside the call.

The invariant the port must keep when either of those changes - a page
faulted out under a transfer, a mapping that moves a frame - is a pin: a count
of the transfers in flight in the frame's record, taken before the PRDT is
programmed and given back in the completion path, including the error path.
It is not in 1.6 because no code of 1.6 can move a frame; it is written here
so that the first lane that can (a swapped page, a `MAP_SHARED` page moved by
`msync`) finds the rule instead of inventing one.

The same rule covers the end of the transfer: the driver reads the status of
the bus-master engine and of the channel *before* it returns, so the memory is
the caller's again only when the device is idle. A second command of the same
channel cannot start while the first one is in flight (the start bit is
cleared and the status read back).

### 4. Coherence

x86-64 is coherent for DMA: the caches of the processor and the writes of a
bus master see one memory, and no `clflush`, `wbinvd` or fence is needed
around a transfer. The driver therefore issues none. What it does need is
ordering of its own accesses to the descriptor table: the PRDT entries are
written before the address of the table is programmed, and the engine is
started after that, which the compiler is not free to reorder across the
`outl` to the bus-master register (`port_outl` is `asm volatile` with the
default memory clobber). The completion path reads the status of the engine
after the interrupt, and the interrupt is the device's word that the last
descriptor was consumed.

### 5. The buffer must be memory

A device may be handed RAM and only RAM: not the framebuffer window, not a
BAR, not a page of the kernel image that is read-only in the tables (the
translation of the image is arithmetic and would happily name it, so the
driver's callers are the guarantee - a block device is asked by a filesystem
with a buffer of the kernel or of a program, and by nobody else).

The block layer's own cache is `kmm_alloc` memory: it is the heap, the
identity map, and the two buffers of the cache (the copy and the staging) are
handed to the driver as they are. Their physical addresses are their
addresses, and the first sector of a run may start at any 1 KiB boundary, so
the split of §2 is what makes a run of sixteen sectors two or three PRDT
entries and not one.

### 6. What a driver does with a machine that has no bus master

Nothing. The discovery of the controller (the class code of the PCI function,
the bus-master bit of its command register, the BAR of the registers) answers
"no" on a machine without one, and the driver keeps the PIO path it had. The
choice is made once, at the boot, and reported: `ata0: bus-master DMA on
0xc000` or `ata0: PIO, no bus master`.

## Alternatives

**A. A bounce buffer for every transfer.** Copy the caller's bytes into a
buffer of the driver that is known to be physical, hand that to the device,
copy back for a read. It needs no translation and cannot corrupt anything.
The price is a copy of every byte in the kernel, which is exactly the cost the
DMA is meant to remove, and it hides the day the copy is forgotten. It stays
as the fallback of last resort: PIO moves the bytes through the port instead
and needs no second buffer at all.

**B. `k_port_dma_map()` in the portable contract.** A kernel-wide
`map/unmap` pair in `kernel/port/mem.h`, implemented by every port. On the
board there is no DMA engine to speak of, on the host there is no device, and
the null port has neither: three implementations of `return ptr`. The
contract would be a fourth place that has to agree with the page tables, and
the page tables are the only place that knows. The translation stays in the
port that has one.

**C. The filesystem translates.** `ext2_write_bytes` knows the offset in the
file and could name the sector; it cannot know whether the caller's pointer is
a page of a program, and it is portable code that must not learn.

**D. Trust the identity map.** True for the heap, false for a program page and
false for the image. The failure is silent corruption, so this is not an
option, it is the bug the ADR exists to prevent.

## Consequences

* `ports/port_x86_64/src/ata.c` speaks bus-master DMA: the controller is found
  by its class code, the bus-master bit is set in its command register, the
  PRDT is a table of 4 KiB pieces, and the completion is the interrupt of
  IRQ 14 instead of a poll of the status register. A command that the
  controller refuses, a buffer that does not translate and a machine without
  the function all end in the PIO path, which is unchanged and still tested.
* The DMA path runs with the interrupts on and holds a mutex of the driver:
  the channel has one command register and the bus-master engine one PRDT, and
  the transaction is no longer a section with the interrupts off. The old PIO
  path keeps its `cli`, so a machine that falls back is exactly the machine of
  1.5.0.
* The wait of a transfer is the interrupt: the task halts the core with the
  interrupts on (`hlt`) and the line of the disk wakes it, with the budget of
  the TSC as the limit and the status register read only when the budget runs
  out. A machine where the interrupt never arrives still finishes the command
  and reports it.
* `docs/memory.md` gains the table of §1 as the statement of the map, and the
  next driver that speaks to a bus master (virtio-blk, AHCI, NVMe) has the
  rule and not an example to copy.
* The write-back cache of the block layer is the other half of the number:
  DMA moves a run of sectors per command, and the cache is what makes a run.
  Its dirty sectors are counted in `/proc/blockdev`, and a `sync` of the
  filesystem empties it before the medium is told to flush its own cache.
  A cache of five hundred boxes cannot be searched by walking it, so it is
  not: the boxes are found through an index of open addressing and the one to
  evict is the hand of the cache. That is the difference between 8.4 and
  14.4 MiB/s of a file written to ext2 on the volume of 5 GiB, and between
  13 and 36 MiB/s of the same file read back.

### What the numbers say about the ceiling

The disk is no longer the limit of this machine, and the measurements are
unambiguous about it:

| What | 1.5.0 | 1.6, this ADR | Where the time goes |
|---|---|---|---|
| ext2, a file of 128 MiB written | 8.2 MiB/s | **14.4 MiB/s** | 57 % the sector operations of the block layer, 3 % the disk |
| ext2, the same file read back | 13.5 MiB/s | **35.9 MiB/s** | the same two, in the other direction |
| MicroFS, 16 MiB written | 3.5-6.2 MiB/s | **8.3 MiB/s** | the chain of the blocks, one sector a step |
| MicroFS, 16 MiB read back | 17.2 MiB/s | **77.3 MiB/s** | the disk |
| 16 MiB through the bus master alone | - | ~500 MiB/s | 31 ms, the copy into the staging buffer included |

The driver moves the 16 MiB of the write in 31 ms and waits for its
interrupts for 12 ms of it; a machine that falls back to PIO writes the same
file at the same rate. What is left is the traffic of the filesystem: the
writer of ext2 asks the device for eleven sectors of 1 KiB for every KiB of
the file (the data, the zeroing of a new block, the bitmap, the descriptor of
the group, the inode), and every one of those is a copy of a kilobyte - under
the emulation of QEMU's TCG a copy of a kilobyte is about two microseconds,
which caps the writer of that filesystem at forty-odd megabytes a second
before its own arithmetic is counted. Eighty megabytes a second of this file
needs fewer sector operations per byte - a block-granular writer, or a device
whose sector is 4 KiB - and that is the lane of ext2, not the lane of the
bus. The target of the plan is not met, and this is why.

On a machine with KVM the same guest code runs four to five times faster, so
the ceiling moves to the same extent; the disk does not: it is the emulated
processor of the host that the number is about.

## What this ADR does not do

* It does not give a program a mapping (`mmap`, M1). When one arrives, the
  frame behind a shared page is translated by the same call and the pin of §3
  becomes necessary the moment a page can move.
* It does not put an IOMMU in the picture. A machine with one would translate
  a *bus* address that is not a physical address, and the contract of the
  driver would gain a second number. QEMU's PC has none.
* It does not move the translation into the block layer. The layer keeps the
  pointer it was given and hands it on; the driver, which is the port, asks.
