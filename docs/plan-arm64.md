# The plan of the ARM port

> A port of Micro OS to ARMv8-A, and only later a phone. The order matters:
> a driver of a phone is the last mile, not the first, and a machine that
> cannot boot has no mile at all.

## Why this is a port and not a rewrite

`components/kernel/` is portable C11 and talks to a machine only through
`kernel/port.h` (ADR 0004). Every arm of the tree already proves it: the host
substrate, the x86-64 machine, the ESP32 board and the null port. A new
`arch/arm64` is the fifth reader of the same contract - and the first with an
MMU that is not x86-64.

The port does not have to follow every release. The board is the precedent:
since ADR 0018 the ESP32 is a compatibility port that receives the cheap
changes and may lag. An ARM port has the same right, written in an ADR of its
own when it lands.

## Stage 0 - the contract of the port (a day)

What a machine must answer, and nothing else: the page tables and the switch
of an address space, the context of a task, the entry into EL0 and the return,
the timer, the controller of interrupts, the console, the block device, the
clock. `kernel/port.h` is the list; the stage is to read it as an ARM
implementer would and write down every place where the contract still speaks
x86-64 (`PORT_KERNEL_BASE`, the identity map, the arena of a program).

Exit: `docs/ports/arm64/contract.md`, and a list of the gaps in `port.h`.

## Stage 1 - a machine that boots (one to two weeks of one agent)

QEMU `virt`, `-M virt -cpu cortex-a57 -m 256M`, ARMv8-A, 4 KiB pages:

* a loader or a `-kernel` image: the kernel at its link address, the device
  tree read for the UART, the memory and the GIC;
* PL011 UART as the console (write, and later read);
* the generic timer as the tick: `CNTPCT_EL0`/`CNTP_CVAL_EL0`, 1 kHz, the same
  `K_SCHED` contract as x86-64;
* GICv2 (`virt` gives v2 for a cortex-a57 machine): the distributor and the
  CPU interface, one handler per line, EOI, the same shape as the I/O APIC of
  1.6;
* EL0: `eret` into a program, `svc` back, one call answered end to end.

Exit: the boot log, the shell prompt, `uname`, one program that prints and
exits; `make arm-smoke` boots the image and checks the answers.

## Stage 2 - the same machine as x86-64 (two to four weeks)

* `virtio-mmio` block (the disk image is the same one the x86-64 tar carries)
  and `virtio-mmio` network, both written against the published specs, no
  disassembly;
* the MMU for a program: a page table per task, the window of the user, the
  fault handler, copy on write - the x86-64 rules of ADR 0007 translated;
* `MOX1` images loaded and run in EL0, `make arm-smoke` at the level of
  `make x86-smoke`.

Exit: `/bin/sh`-shaped shell, `cat`, `ls`, `pkg` against a local volume.

## Stage 3 - a real board (after stage 2, not before)

A board whose documentation is public and whose boot can be replaced: a
Raspberry Pi, a Rockchip board, or a device of the postmarketOS list. The
first target is the one where the bootloader is replaceable without a fight.

## Stage 4 - a phone, and what "driver" means there

A phone is not one device: a DSI panel with its init sequence, a touch
controller, a PMIC with its regulators, a GPU, a modem, a camera, sensors -
each one a controller of its own, often behind TrustZone and a locked
bootloader.

The rule for this stage: **read the GPL sources before reading a
disassembler.** Mainline Linux and postmarketOS carry drivers for hundreds of
models as source; the register sequences, the timings and the order of an
init are knowledge, and knowledge is not copyright - only the expression is.
A driver written from that knowledge is ours and stays MIT.

Disassembly is the tool for a block that has neither source nor
documentation, and it comes after a machine boots, a shell answers and a
program runs. Never before.

## The cheap decision (a day or two, any time)

Before stage 3 there is a stage that costs almost nothing and answers "is this
model even possible": a dossier of one model - its SoC, what mainline and
postmarketOS already have for it, whether the bootloader can be replaced,
what is behind TrustZone, and what the first three blocks would be. That is a
day of reading, and it turns a hope into a decision.

## What is deliberately not in this plan

* a rewrite of the kernel for ARM (the contract is the point);
* a driver of a phone before stage 2 is green;
* an emulator of a phone in QEMU (the `virt` machine is the emulator);
* 32-bit ARM, big-endian, and a machine without an MMU.
