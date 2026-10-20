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

## Measured on the device

The first version of the scheduler was verified on a real StickC Plus2:

```
MicroOS 0.1.0 (build Oct 21 2026 01:36:03)
board: stickc_plus2 — M5Stack StickC Plus2
arch: xtensa-esp32, compiler: gcc 13.2.0
[ ok ] sched
[ ok ] board
init: system is up, pid 1, uptime 0 ms
selftest: 3 task(s), 15 context switch(es)
  pid  prio  state     name
    1     5  running   init
    2     5  zombie    selftest0
    3     6  zombie    selftest1
    4     7  zombie    selftest2
selftest: PASS
```

Memory footprint of that build: 102 KB of flash code, 58 KB of IRAM, 22 KB of DRAM
and the 2 MB PSRAM pool registered in the heap.

## Side findings of phase 1F

The hardware doctor found three bugs that only a real device can reveal:

1. **The I2C driver of IDF 5.3**: the last argument of `i2c_master_transmit()`
   is a timeout in milliseconds, not a flag. The value `true` meant 1 ms, and
   the transactions were cut short depending on the phase of the boot. With a
   timeout of 100 ms the bus works reliably.
2. **SPI through the GPIO matrix** is limited to 26.7 MHz, 40 MHz fails with
   `assigned clock speed not supported`. The display runs at 20 MHz.
3. **The SPI DMA of the ESP32 cannot read from the PSRAM**: the framebuffer is
   sent to the panel in 4 KB pieces through an internal buffer.

Lesson: the hardware doctor is not a formality but a working tool. Each of
these bugs looked like "the device is broken", while the code was wrong.
