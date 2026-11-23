# ADR 0012 - More than one core

* Status: accepted, **the gate of 1.0 was withdrawn**; the first phase is
  implemented after 1.0.0 (see "The pilot of 1.x")
* Date: 2026-11-21
* Updated: 2026-11-21 - multi-core is after 1.0.0, not a gate of it (see
  "The gate, changed")
* Updated: 2026-11-22 - the shape, the host and the x86-64 processor list are
  in the tree; the AP bring-up is the blocker (see "The pilot of 1.x")
* Updated: 2026-11-23 - the AP bring-up is done on x86-64: two cores run, the
  smoke of the port passes 34 of 34 and the two-core session soaks for 600
  seconds. The remaining work is named at the end of the section


## Context

The kernel was written for one core. `k_port_enter_critical()` turns the
interrupts off, which on one processor is a lock; the scheduler keeps one
`current`, one ready queue and one idle task; the port contract has one
console, one clock and one heap. That is not a bug on the ESP32-C3 or on a
single-core QEMU machine, but the ESP32-PICO-V3-02 has two cores, the x86-64
machine of QEMU can have more, and the host port already runs every task in a
`pthread`.

The first decision of this ADR made multi-core a gate of 1.0. It was
withdrawn on the same day (see "The gate, changed"): the promise of 1.0 is a
stable single-core system, and the second core is a project of 1.x. The shape
below is still the plan, not a wish list.

## Decision

### 1. The shape

* A `struct k_cpu` per core: the current task, the idle task, the run queue,
  the tick count, the interrupt state, the kernel stack of the idle loop, the
  GDT/IDT/TSS of the port, the thread pointer of the TLS and the identifier
  the hardware gives it.
* The scheduler is per-CPU: a task belongs to one run queue at a time and is
  picked by the core that owns it; a task is moved only by the scheduler, under
  the lock of both queues.
* A global scheduler lock protects the queues and the task table. It is a
  spinlock with the interrupts of the local core saved and turned off, and it
  is never held while the kernel sleeps or copies to a user.
* `k_mutex` stays a sleeping lock; the allocator (`kmm`), the VFS and the
  console keep their own locks, which become interrupt-safe spinlocks or
  sleeping mutexes according to what they protect. A lock that is held while
  another core can take an interrupt is a bug, not a tuning choice.
* `KERNEL_SMP` is a compile-time flag (ADR 0010). With it off the same sources
  build the one-core kernel of today; with it on the kernel may use every core
  the port reports.

### 2. The machines

**x86-64.** The BSP brings up the APs: ACPI/MADT or the multiboot information
for the list, a trampoline in low memory, `INIT`/`SIPI`, a stack and a GDT/IDT
per AP, the local APIC timer, the I/O APIC for the lines, and an IPI for the
TLB and for a task that has to run elsewhere. Until the APIC is there the
machine boots on one core and says so. `-smp 2` is the smoke configuration.

**ESP32.** The two cores are already there under FreeRTOS SMP (ESP-IDF 5.3).
The port creates its tasks without a core affinity, the critical section maps
to a spinlock with the interrupts saved, and the kernel's scheduler must not
assume that the task it saw running a moment ago is still on this core. The
hardware doctor and the drivers keep their own rules, but no driver may assume
one core.

**Host.** One `pthread` per task is already the model; it is the place where
TSan sees the races of the kernel before the board does. A multi-core host run
is not a new port, it is the same one with more threads.

### 3. What is explicitly allowed to be coarse

* one lock around the scheduler's slow paths;
* one lock around the block layer;
* one lock around the console;
* a task that is not migrated while it runs (the next task may be on another
  core, the current one does not move in the middle of its slice).

A global lock that every system call takes is **not** allowed: the point of
the second core is that a task that sleeps in the filesystem does not stop the
other core from running a program.

### 4. The gate

A 1.0 build on x86-64 boots at least two cores, runs the shell and a program
on them, and passes the host TSan run and a two-core soak without a deadlock,
a lost wakeup or a task that runs on two cores at once. The ESP32 build runs
its tasks on both cores and the doctor does not report a core it did not see.
The board release notes say how many cores the image uses.

## The gate, changed

The project first promised correctness on two or more cores for 1.0, and this
ADR made it a gate. On 2026-11-21 the decision was changed:

* **Multi-core is after 1.0.0.** It is the largest single project left (a
  per-CPU scheduler, the allocator, the VFS, the console and the block layer
  under two cores, and for x86-64 an AP bring-up with APIC and ACPI), and it
  is not a change of an ABI, a format or a default: a system that boots, runs
  and is tested on one core is a correct 1.0, and a second core arrived later
  does not invalidate what a 1.0 program or volume expects.
* **What 1.0 promises** is a correct single-core kernel on both machines; the
  two cores of the board are used by FreeRTOS but the kernel scheduler is not
  asked to run a task on both at once.
* The shape, the machines and the price below stay as the plan of a 1.x
  release. `KERNEL_SMP` stays off by default and is not a gate.
* The 1.0 checklist keeps the lines of section 9 as the plan of 1.x, marked as
  not a gate.

The reason is not that the work is small; it is that the work is large and the
promise of the release does not need it. A gate that is moved is written down,
not quietly dropped.

## The pilot of 1.x, closed (2026-11-23)

The first phase of the plan began on 2026-11-22 and stopped at the line QEMU
draws: the shape and the host gate were in the tree, and the AP bring-up was
the blocker. The work closed on 2026-11-23, and the machine of two processors
is the gate of 1.3.

* **The shape.** `struct k_cpu` (kernel/cpu.h): the current task, the idle
  slot, the tick count, the nesting of the interrupt state, the kernel stack,
  the thread pointer, the tables of the port and the number of the core.
  `KERNEL_SMP` (default off, ADR 0010) builds one core or `KERNEL_CPUS_MAX`
  (four) from the same sources; with it off the array has one slot and the
  kernel is the kernel of 1.1.
* **The scheduler is per-CPU, and a core picks only from its own queue.**
  Every core has a run queue per priority, a task has one `cpu` field, and
  only the scheduler writes it: a new task goes to the next core of a round
  robin, a woken task to the core it already had, and `k_sched_migrate()`
  moves a task that is not running. The pilot let a core take a ready task
  from another queue; the implementation removed it, because this kernel
  keeps the task that runs in its queue and marks it READY for the few
  instructions between the end of its quantum and the switch. A second core
  that took it in that window would resume a stack that is still in use: a
  task on two cores at once, which this ADR forbids. The claim
  (`k_sched_claim_next()`) chooses the task and marks it RUNNING on the
  calling core in one critical section, so the window between the choice and
  the switch is closed as well.
* **The lock.** `k_cpu_lock()` is the critical section of the port - saved
  flags, interrupts off, one spinlock of the machine - plus the kernel's own
  nesting count. It protects the queues, the sleeping list and the current
  task, and it is dropped before the tick charges a task, before a sleep and
  before a switch. The console (`k_port_write()`) is under the same lock for
  the whole call: with two cores the bytes of a line were interleaved
  otherwise. The kmm, the VFS and the block layer keep their sleeping locks;
  the audit is K8 of [debts.md](../debts.md).
* **The host is the gate.** `MICRO_OS_CPUS` (default 1) tells the host port
  how many cores the kernel has; `make test-smp` runs the whole suite on two
  of them, `make test-tsan` and `make soak-host` do the same under the thread
  sanitizer. The tests cover the queues, the wake-up, the migration, the
  claim and the one failure this ADR names: a task that is the current of two
  cores is refused and named by `k_sched_validate()`.
* **x86-64 starts every processor the firmware lists.** The port keeps the
  MADT list with the hardware identifier of each processor, gives every core
  a 64 KiB stack, sends INIT and the start-up IPI through the local APIC to
  the trampoline the loader carries at `0x7000` (parameters at `0x8000`:
  CR3, stack top, C entry, core number), and waits for the core to answer
  from C. The numbers are printed: `2 cpu(s) reported by the firmware, 2 in
  use (apic 0xfee00000)`, and a core that does not answer is counted out and
  named. The core waits before the scheduler exists and `kmain()` releases it
  after `kernel_boot()` has made the array of the kernel.
* **One identity per core.** IA32_GS_BASE points at the record of the core
  whose first word is its kernel number; `k_port_cpu_id()` is `movl %gs:0`.
  The boot is found by CPUID (leaf 1, EBX[31:24]) and keeps the number 0; the
  other processors follow the MADT. The boot's `TSS.RSP0`, the thread pointer
  of the TLS and the current context are per core for the same reason.
* **The clock is one, the timers are per core.** The PIT of the core 0
  advances the millisecond of the kernel; an AP takes its ticks from its own
  local APIC timer (1 kHz, calibrated against the TSC; QEMU counts at about
  62.6 MHz with divider 16). The 8259 is wired to the boot processor and its
  LINT0 is ExtINT only there: with the external mode open on every core, QEMU
  delivered the PIT to either of them (1889 ticks on the core 0 and 4933 on
  the core 1 of 6822) and the clock ran at a third of real time.
* **An exit gives the core back.** A task that ends with nothing else to run
  returns the core to its boot context (kmain on the core 0, the idle loop of
  an AP) instead of waiting in the context of a dead task, whose slot the
  parent may reap at any moment; the idle context claims the next task.
  `CR0.WP` is set on every core, because it is a register of the core and the
  kernel's copy into a read-only page of a program has to fault there too.
* **What is still open.** The I/O APIC is a stub: the 8259 still delivers the
  timer and the keyboard to the core 0, and the lines are not routed through
  the I/O APIC, so a machine whose firmware delivers nothing to the legacy
  chip has no clock on the APs (the host and QEMU do). The IPI vector exists
  and `smp ping` proves it, but the scheduler does not send it yet: an idle
  core wakes on its own timer, at worst a millisecond later, and a core does
  not preempt a lower-priority task of another queue. The FPU and SSE state
  is not saved at a switch (the kernel and the programs are built
  `-mgeneral-regs-only`), and there is no guard page under a task stack. The
  board stays out of this release, as ADR 0018 says.

## Consequences

**Good**

* The second core of the board is used, and the x86-64 port stops being a
  single-core exercise.
* The host port already has the concurrency; TSan becomes a gate instead of a
  target of its own.

**Bad**

* Every global becomes a decision: a per-CPU variable, a lock or a constant.
* The x86-64 port grows APIC, ACPI and a trampoline, which is a project of its
  own before the first task runs on the second core.
* A coarse lock can hide a scalability problem for a long time; the answer is
  a measurement in the release, not a promise.
