# ADR 0034 - The interrupts of many cores and the wake of a core

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0012 (more than one core), ADR 0007 (the map of the port),
  `docs/debts.md` K8, `docs/plan-1.6.md` M4, `kernel/port/cpu.h`,
  `kernel/port_x86_64_apic.h`

## Context

ADR 0012 made the second core real and left two debts that the pilot of 1.3.0
named instead of hiding: the I/O APIC was a stub, and the scheduler did not use
the inter-processor interrupt it already had.

The first debt was the interrupt controller of the machine. The 8259 of a PC
has one output, and the firmware wires it to the external pin of the local APIC
of the boot processor: every line of the machine reached the core 0 and only
that one. The timer of the kernel is the PIT, which is a line of the 8259, so a
machine whose firmware describes no legacy chip has no clock at all; and the
keyboard, the disk and the network of the port are all on lines the core 0
takes. The APs lived on their own local APIC timer alone.

The second debt was the wake. A task that the scheduler put in the queue of
another core was started by that core's next timer tick - at most a millisecond
later, and a millisecond is ten times the cost of a switch on this machine. A
core that was halted in `hlt` with an empty queue did not know that work had
arrived until its timer woke it for nothing.

Both are the same subject: an interrupt is the only way a core learns about
anything, and the port had one controller that could reach one core.

## Decision

### 1. The lines go through the I/O APIC, and the 8259 is the fallback

`src/ioapic.c` programs the redirection table of the chip the firmware names in
the MADT (`src/cpu.c` reads the table: the address of the first I/O APIC and
the interrupt-source overrides of the ISA lines). One entry per line: the
vector, the destination core as the identifier of its local APIC, the polarity
and the trigger the override names, and a mask bit. Every line starts masked
and the port opens the two it has a handler for - the timer of the PIT on the
vector `0x20` and the keyboard on `0x21`, both on the core 0 - so a line nobody
programmed cannot raise a vector of the table that has no body.

The 8259 is not removed. A machine whose firmware describes no table gets the
fixed address `0xFEC00000` and the identity of the lines 0, 1 and 2, which is
what every PC since 1995 has; a chip whose window answers nothing (no device,
or a page that takes the write and keeps nothing) makes
`port_x86_64_ioapic_init()` answer false, and the port keeps the 8259 exactly
as 1.5.0 had it. The order of the bring-up is what keeps a line from arriving
twice: the legacy chip is masked before the table is opened, and opened again
if the chip did not take both lines.

The end of an interrupt follows the chip that raised it: the EOI register of
the local APIC of the core that took the line when the I/O APIC delivered it,
the EOI of the 8259 when the legacy chip did. The body of a vector calls
`port_x86_64_irq_eoi()` and does not know which of the two it is.

The local APIC of the boot processor is enabled by the interrupt bring-up and
not only by the bring-up of the processors, because a line the I/O APIC routes
is a message to it: a build without `KERNEL_SMP` runs the same table.

### 2. The scheduler wakes a core with an interrupt

`kernel/port/cpu.h` gains one call:

```c
void k_port_cpu_wake(k_u32_t cpu);
```

The scheduler calls it when it has put a task in the queue of a core other than
the one that runs the caller - a new task of the round robin, an unblock, a
sleep that expired, a migration - and it calls it **after** the scheduler lock
is dropped. The order is the decision and not a detail: the core that takes the
interrupt claims from the same queue under the same lock, and a command sent
with the lock held would leave it spinning for the length of that section.

The call has a weak default in `components/kernel/src/cpu_default.c` (nothing
to do), so a port with one core, or a port written before this ADR, keeps the
behaviour of 1.5.0: the task waits for the next tick of its core. The x86-64
port sends the IPI vector `0x51`, and the body on the other side is the one
`smp ping` already exercised: claim the next task of this core and switch to it
if it is not the one that runs. The claim chooses by priority from the top of
the queue, so a task of a higher priority preempts the running one of that
core, and one of the same priority takes its turn.

The body refuses one window that the tick also refuses: a task that has left
the scheduler - `k_sched_task_terminated()` cleared the record of the core -
but has not reached `k_port_task_exit()` yet. Its stack and its address space
are published to the next context of the core a handful of instructions later,
and a switch in that window abandons the context that was about to publish
them. The tick has refused it since 1.3.0; the interrupt did not, and the
600-second session of this lane found it as a heap that fell 512 KB in three
minutes and a `fork` that answered `ENOSPC` after 160 cycles of load: one
interrupt per signal, one task of `/bin/signals` in the window, 32 KiB a time.

### 3. The clock of a core can be stopped, and only for the experiment

The claim of the debt is that a task starts on a core *before its tick*. A
measurement of "how much earlier" would be a number of microseconds that
nobody can attribute; the experiment is the stronger statement: stop the clock
of the core, and the only way a task can begin there is the interrupt of
another core.

The request rides on the same vector: a word per core that the body of the IPI
takes and clears (`PORT_X86_64_REQUEST_TIMER_OFF`/`_ON`), and the flag
`port_x86_64_timer_off(cpu)` that the core writes when it has done it. The
shell command `smp wake` stops the clock of the other core, creates four tasks
(which the round robin puts on both cores), waits for them, starts the clock
again and prints the counters. A core left without a clock is a machine that
takes no work, so the command starts it again before it returns, whatever the
tasks did - and the smoke test checks that it did.

### 4. The evidence is a counter and a report, not a sentence

The port counts, per core, the tasks that began in the body of the IPI (that
is, before the core's timer could have offered them) and the ones that began at
its tick, with the tick of the core at the last wake. The kernel counts the
tasks it handed to another core and keeps the core and the tick of the last one
(`k_sched_remote_wakes()`). `smp` prints the pair, `smp irq` prints the table of
the I/O APIC, the vector and the core of each line and the count of the two
lines per core, and `smp irq keyboard <cpu>` moves the keyboard to another core
so that a run can see the interrupt arrive where it was sent.

`make x86-smp-smoke` reads all of it: the timer arrives on the core 0 and
nowhere else, the four tasks of `smp wake` start on the other core with its
clock stopped, its ticks do not move while the experiment runs, and the keys
the monitor of QEMU presses arrive on the core the keyboard line was sent to.
The gate is 52 checks, and the 600-second `make soak-smp` session is the other
half of it: the counters are the proof that the path is taken and the session
is the proof that taking it does not corrupt anything.

## Consequences

**Good**

* The timer and the keyboard of the machine are no longer tied to the core of
  the boot, and a machine whose firmware delivers nothing to the legacy chip
  has a clock. A line can be sent to any core, which the disk and the network
  of a later release need.
* A task that another core hands to a core starts in microseconds, not in a
  millisecond, and an idle core is woken by the interrupt instead of by its
  timer.
* The port keeps one path for both controllers: the vectors, the bodies and the
  counters are the same, so the fallback is not a second system to test - the
  whole smoke of the port runs it when the chip is missing.

**The price**

* Two stores to the I/O APIC window per route, and one IPI per remote enqueue.
  The interrupt is a few hundred nanoseconds of the sender and a claim of the
  receiver; on a machine whose quantum is ten milliseconds nobody sees it, and
  a port that wants the last cycle back can compare priorities before sending.
* The contract grows a call. It is weak, so nothing breaks, but a port that
  reports more than one core and does not implement it is a port whose tasks
  wait for the next tick - which the boot of such a port does not say.

**What is still open**

* The guard page under the stack of a task is the third item of M4 and is not
  in this ADR.
* A machine with several I/O APICs keeps the first; the lines of a second chip
  are not programmed, and no bus of the port needs one yet.
* A level-triggered line whose device needs its entry masked while it is served
  (the usual shape of a shared PCI line) is not programmed: the two lines of
  the port are edge triggered, and the disk and the network are still polled.
  The mask bit and the two calls that set it exist for the day they are not.
