# ADR 0012 - More than one core

* Status: accepted, **the gate of 1.0 was withdrawn**; the first phase is
  implemented after 1.0.0 (see "The pilot of 1.x")
* Date: 2026-11-21
* Updated: 2026-11-21 - multi-core is after 1.0.0, not a gate of it (see
  "The gate, changed")
* Updated: 2026-11-22 - the shape, the host and the x86-64 processor list are
  in the tree; the AP bring-up is the blocker (see "The pilot of 1.x")


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

## The pilot of 1.x (2026-11-22)

The first phase of the plan is in the tree, and it stops at the line the
machine of QEMU draws:

* **The shape.** `struct k_cpu` (kernel/cpu.h): the current task, the idle
  slot, the tick count, the nesting of the interrupt state, the kernel stack,
  the thread pointer, the tables of the port and the number of the core.
  `KERNEL_SMP` (default off, ADR 0010) builds one core or `KERNEL_CPUS_MAX`
  (four) from the same sources; with it off the array has one slot and the
  kernel is the kernel of 1.0.
* **The scheduler is per-CPU.** Every core has its own run queue per priority,
  a task has one `cpu` field, and the field is written only by the scheduler
  (create, wake, claim, migrate). A core that has nothing of its own takes the
  head of the best queue of another core when that task has a higher priority;
  that is the only migration, and a running task is never moved - the promise
  of this ADR. `k_sched_migrate()` is the call, and it refuses a running task.
* **The lock.** `k_cpu_lock()` is the critical section of the port (saved
  flags, interrupts off, a lock of the machine on the ports that can have
  another core) plus the kernel's own nesting count. It protects the queues,
  the sleeping list and the current task, and it is dropped before the tick
  charges a task, before a sleep and before a switch. The kmm, the VFS, the
  console and the block layer keep their sleeping locks and their critical
  sections; the audit of what they need under two cores is in
  [debts.md](../debts.md) (K8).
* **The host is the gate.** `MICRO_OS_CPUS` (default 1) tells the host port
  how many cores the kernel has; `make test-smp` runs the whole suite on two
  of them, `make test-tsan` and `make soak-host` do the same under the thread
  sanitizer. The port gives every task a pthread, so the tasks really run in
  parallel; a "core" is a queue and an identity of the kernel, and a task is
  on one of them because it is one pthread with one field. The tests check
  that the port and the record agree, that a task is in one queue, that a
  wake-up is not lost and that the queues stay consistent while they move.
  TSan found three pairs of the new code (the `cpu` field of a task, the
  `current` of a core, the mark of a signal) and they are fixed with the same
  lock or an atomic read; the 20-second soak of two cores runs a million
  operations and six thousand forks with no error. The first five-minute soak
  then reported one "inconsistency of the queues" in ten million operations:
  the switch of this kernel keeps the running task in its run queue until its
  quantum, a sleep or a block (that is 1.0), and the check of the queues had
  been written as "a task in a queue is ready". The check now says what the
  kernel means - a queue holds the ready tasks of a core and the task that
  runs there - and it names the task, its state and its core when it fails.
  The eight-worker run of two minutes (18 million operations, 92 thousand
  forks) and the five-minute run of the thread sanitizer after the change are
  clean.
* **x86-64 reads the processors and starts one.** The port finds the root
  pointer of ACPI (the extended BIOS area, then the last kilobyte of the BIOS),
  walks the RSDT or the XSDT, reads the MADT and remembers the local APIC.
  The boot prints both numbers - the processors the firmware calls enabled and
  the ones that run - so a machine of two processors says `2 cpu(s) reported
  by the firmware, 1 in use`. `make x86-smoke` boots with `-smp 2` and reads
  that line; the kernel of `KERNEL_SMP=ON` builds and boots
  (`make x86-smp-build`) and the portable side of the second core is exercised
  on the host, not on the machine.
* **The blocker of the AP bring-up** is not the MADT. It is, in order:
  (1) the critical section of the port is `cli` and a depth counter, which
  stops the local core only - every kernel structure that trusts it would need
  a real spinlock in it (a global one, with the interrupts saved) before a
  second core may enter the kernel at all; (2) the port's `g_current`, the
  TSS (RSP0 and IST) and the thread pointer are singletons per machine and
  need one set per core; (3) the trampoline in low memory (real mode, GDT,
  PAE, long mode, a stack, the entry into the kernel), INIT-SIPI-SIPI through
  the local APIC, and its MMIO page mapped; (4) the local APIC timer and its
  vector, because the PIT and the 8259 deliver to the boot processor only, and
  the I/O APIC for the keyboard and the disk. Doing (1)-(4) half way would
  boot a second core into a kernel that can corrupt its own tables, so the
  pilot stops before it and the numbers say so. The work is the plan below,
  not a claim.
* **The board is untouched**, as ADR 0018 says: its two cores run under
  FreeRTOS and the kernel scheduler of that port is not asked to use both.
  The weak default of `kernel/port/cpu.h` answers "one core" for a port that
  predates the calls, which is what the board does until its own work.

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
