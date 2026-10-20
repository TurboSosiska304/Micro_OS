# ADR 0002 - Execution substrate: our kernel on top of FreeRTOS

* Status: accepted (phase 1 in progress)
* Date: 2026-10-21

> Russian version: [0002-scheduler-substrate_ru.md](0002-scheduler-substrate_ru.md)

## Context

For a Linux-like process to be preempted at an arbitrary point, the scheduler must
be able to switch context. On the ESP32 there are two ways:

1. **FreeRTOS as substrate.** Every kernel task is a FreeRTOS task, priorities map
   one to one, and FreeRTOS performs the context switch. Our scheduler becomes a
   "meta scheduler": it decides *which* of the FreeRTOS tasks should be active,
   while the actual switch is done by FreeRTOS.
2. **Our own scheduler on top of FreeRTOS.** We use a single service task (or an
   ISR trigger) and perform the context switch ourselves: save and restore stack
   pointers and registers.

## Decision

We start with **option 1**: a kernel task is a FreeRTOS task, one to one. This
gives preemption "for free", a proven context save/restore implementation for
Xtensa, and leaves the WiFi stack untouched.

Option 2 (our own scheduler) moves to a later stage, when we need what FreeRTOS
cannot give us:

* several kernel tasks separated by priority levels *within* one FreeRTOS quantum
  (Linux-like fairness between processes);
* "system calls" that switch context at an arbitrary point;
* exact accounting of quanta and CPU time.

The switch is implemented via `esp_cpu_set_pc`/`portYIELD` with manual saving of
the Xtensa registers (the same way FreeRTOS does it for xPort).

## Consequences

**Good:**

* Working multitasking already in phase 1, without risky low-level tricks.
* WiFi/BLE/TLS keep working: they live in FreeRTOS tasks.
* A bug in the scheduler does not end in "a hang in IRAM" — the FreeRTOS watchdog
  recovers the task.

**Bad:**

* The exact priority behaviour does not match Linux: the FreeRTOS scheduler
  switches tasks by its own rules (time slicing), with our priority level on top.
* Two priority levels (FreeRTOS and ours) can conflict: the FreeRTOS priority of
  a kernel task has to be chosen carefully.
* Moving to our own scheduler is separate work involving Xtensa register saving;
  mistakes there are unpleasant (a silent portrait of garbage on the stack).
