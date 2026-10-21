# Phase 1 - Tasks and the scheduler

The first real part of the kernel: the tasks, their states, the priorities and
the time slices.

## What is there

* `struct task` with the number, the name, the state, the priority, the stack
  and the wake up tick.
* The states: `new -> ready -> running -> sleeping/blocked -> zombie`. A
  finished task stays a zombie until it is reaped, like in Linux.
* The run queue of every priority level and the queue of the sleeping tasks.
* The time slice: a task that used up its quantum goes to the end of the
  queue of its priority.
* `task_wait()` with the semantics of `waitpid()`, the parent and the
  children, the table of the open files and the working directory of a task.
* The execution context belongs to the port: a FreeRTOS task on the board, a
  pthread on the host.

## The mistakes that cost time

* **The run queue could lose a task.** The queue had 8 entries while the
  system allows 32 tasks. When the queue was full, the oldest entry was
  dropped silently: the task stayed in the state "ready", but nothing would
  ever pick it up. The capacity is now `TASK_MAX_TASKS`, a task is added at
  most once, an overflow is reported, and `k_sched_validate()` checks the
  invariant. The regression test creates 20 tasks of the same priority and
  makes them ready at the same time - with the old queue it fails.
* **A block or an unblock could get lost.** A task that waits with a timeout
  is transiently in the state "sleeping", and a `k_sched_unblock()` in that
  moment was ignored. The request is remembered now and applied when the task
  wakes up.
* **The uptime was counted in ticks.** The tick of the kernel cannot keep up
  when the processor is busy (the start of the WiFi takes seconds), so the
  uptime was a quarter of the real time. The uptime comes from the real clock
  of the board now, the ticks are a separate value.
* **`argv` was not terminated.** A command without arguments read the memory
  behind the list of the arguments. The shell repeated the whole line after
  the Enter; now it repeats every character while it is typed.

## What is worth remembering

* The kernel lock is a **recursive** mutex: the spinlock of FreeRTOS is not
  reentrant, and the core calls the locked helpers from inside a locked
  section.
* The test runner of the unit tests is itself a task. The tests then use
  the same API as the programs on the board.
