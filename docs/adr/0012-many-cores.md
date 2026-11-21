# ADR 0012 - More than one core

* Status: accepted
* Date: 2026-11-21

## Context

The kernel was written for one core. `k_port_enter_critical()` turns the
interrupts off, which on one processor is a lock; the scheduler keeps one
`current`, one ready queue and one idle task; the port contract has one
console, one clock and one heap. That is not a bug on the ESP32-C3 or on a
single-core QEMU machine, but the ESP32-PICO-V3-02 has two cores, the x86-64
machine of QEMU can have more, and the host port already runs every task in a
`pthread`.

1.0 promises a stable system, and a system that cannot use the second core of
the board it ships on is not the system the board is. Multi-core is a gate of
1.0, not a feature after it. What 1.0 promises is **correctness on two or more
cores**, not linear scalability: a kernel lock around a path that is not hot is
an honest first step.

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
