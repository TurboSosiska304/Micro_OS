# ADR 0046 - The wake that preempts, and the arm under the lock of the object

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0012 (more than one core), ADR 0034 (the interrupts of many
  cores and the wake of a core), `docs/debts.md` K15, `docs/bench.md`,
  `kernel/wait.h`, `kernel/sched.h`, `kernel/port_task.h`,
  `docs/roadmap-to-2.0.md` (the lane of the scheduler)

## Context

Two defects of the same seam were measured, not guessed.

The first was the **latency of a wake**. `k_wait_wake()` moved the wake tick of
the sleeping waiter to the present, and the timer did the rest: the task left
the sleeping table at the next tick, at most a millisecond later. The round
trip of a byte through a pipe - one write, one wake, one read, one ack - was
therefore **1.006 ms** on x86-64 and on the host (`docs/bench.md`, the row
`lat_pipe_p50`). A wildcard thing to pay for a byte that is already in the
kernel's ring.

The second was the **window of a lost wakeup**. `k_wait_block()` registered the
waiter *after* the caller had looked at the state of the object. A writer that
ran in that window found no record of the reader and its wakeup was lost: with
a wait that had no deadline the reader slept for as long as the machine was up.
1.8 answered that with a bound and not with the cause - a pipe and a `poll(-1)`
waited twenty milliseconds at a time (`KFILE_WAIT_CHUNK_MS`) so that a lost
wakeup cost a turn of the loop instead of a line of the shell
(`docs/debts.md`, K15).

The two are one subject: the kernel had no way to say "this task is waiting on
this object, and it is waiting *now*", and no way to make a waiting task run
before the next tick.

## Decision

### 1. A wait is armed under the lock of its object

The API is three calls and the lock of the object held across the first one
(`kernel/wait.h`):

```text
lock(object);
if (the state is already the one I want) { unlock; answer; }
k_wait_arm(channel, second);      /* still under the lock */
unlock(object);
rc = k_wait_sleep(timeout);
k_wait_disarm();
```

The guarantee is one sentence: **the state is changed and the record is armed
under the same lock, and the wake is sent after the state has changed.** A wake
that arrives after the arm finds the record; a wake that arrives before the arm
has changed a state the look under the lock could not have seen yet. There is
no interleaving in which a wakeup is lost, so **a wait needs no bound and no
chunk**, and `KFILE_WAIT_CHUNK_MS` is gone.

The arm is also the second half of the same window on the side of the sleeper:
`k_sched_sleep_unless(ms, &record->woken)` looks at the flag and marks the task
as sleeping in one critical section, so a wake that arrives between the look at
the record and the sleep does not put the task to sleep at all - it answers
WOKEN at once. The two halves close K15 from both sides, which is why the case
of the window can be a test and not a hope: `tests/cases/test_wait.c` hands the
CPU back between the arm and the sleep and wakes the channel in that moment.

`k_wait_block()` is removed. It was the only door of the kernel that could
register a waiter after a look, and a caller that reached for it would rebuild
the defect.

### 2. The wake moves the task, and the scheduler tells the core

`k_wait_wake()` no longer moves a tick. For every waiter of the channel it sets
the record, and `k_sched_wake_task_locked()` takes the task out of the table of
the sleepers, makes it what its sleep was going to make it (READY, except for a
task that was blocked before it slept), puts it in the run queue of its core
and marks that core in the mask of the hand-overs. After the scheduler lock is
dropped the kernel sends the interrupts of the mask with `k_sched_wake_flush()`
- the IPI of ADR 0034 - so a waiter of another core starts in the time of an
interrupt and not of a tick, exactly as a task that another core hands over.

A waiter of *this* core needs no interrupt: the scheduler of this kernel is
cooperative between the calls of the kernel, and the writer of a pipe blocks on
its ack in the next handful of instructions - which is where the woken reader
is picked. What is **not** promised is the preemption of a running task of the
same priority by a woken one; that is a property of the syscall return and not
of the wake, and it is a lane of its own (`docs/debts.md`). The name of the
lane is the promise it keeps: the wake *preempts the tick*, not the task.

The tick keeps its own wake for the timeouts and the signals, and it now
crosses the same door as the channel: a task whose deadline passed (or whose
signal has to be answered) is cut out of a longer sleep of the port as well.

### 3. The port has a door, and a port that cannot answer says so

Three ports of the tree give their tasks to another scheduler, and only one of
them can be cut by the kernel:

* **x86-64** looks at the state of the sleeping task on the way back from its
  scheduler and returns the moment somebody changed it (`sleep_is_over`), so
  `k_port_task_sleep_cuttable()` answers true and no door is needed.
* **the host** sleeps a pthread in `nanosleep`, which nothing outside the
  thread can end. The sleep is now a wait on the condition variable of the
  task, and `k_port_task_wake(task)` sets its flag and signals it. A task that
  is not in the port's list of live contexts has no sleep to cut, and the wake
  is dropped - the list, its lock and the cleanup of a thread make the door
  safe against a task that is exiting at the same moment.
* **the board** is a compatibility port (ADR 0018) and keeps the behaviour of
  1.8: `k_port_task_sleep_cuttable()` answers false through the weak default of
  `src/port_task_default.c`, and the wait of such a port looks at its record
  every ten milliseconds (`WAIT_CHUNK_MS`). The chunk is the granularity of a
  port without a door and not a property of the wait; a port whose sleep a task
  notification can cut answers true and pays nothing.

The two calls are weak, like the two of the cores (ADR 0012): a port written
before them links and behaves as 1.8 did. The contract is raised to 1.13 and
the four ports of the tree say so.

### 4. A sleep of the kernel is never shorter than its time

The door is a nudge and a nudge can arrive for a sleep that has already ended,
and a port rounds its timeout. A sleep that trusted the port once would then be
a sleep of nothing, and the first gate of the release under valgrind found
exactly that: `session_authenticate()` delays a wrong password by twenty
milliseconds and the case asks for sixty, and the call answered less.

`k_sched_sleep_unless()` is a loop on the **clock of the port**, not on the
tick of the scheduler. The deadline is a number of microseconds of
`k_port_uptime_us()` taken on the way in; each round sleeps what the clock says
is left, rounded up to the milliseconds a port takes, and only the kernel ends
the sleep early: the record of a wait has been woken (`stop`, and with it the
signal the wait has to answer with EINTR), a block or an unblock arrived while
it slept.

The clock and not the tick is the whole of the second half of this debt. The
host runs its clock in a thread of its own beside the tasks, and under a slow
runner - valgrind, a loaded machine - that thread takes many ticks in one
millisecond: a loop that counted ticks saw its deadline pass in a tenth of the
time and answered a sleep of a hundred and fifty milliseconds in twelve. A tick
is the unit of the scheduler (the quantum, the charge, the deadline the port is
woken at), and a millisecond of a caller is the unit of the port's clock; the
two agree on a machine that is not overloaded and must not be assumed to agree
on one that is.

The state of a task is not a reason to end a sleep either. A wake of another
object, or a tick that fired early, puts READY in the record of the sleeper,
and the loop takes the task back to SLEEPING for the rest of its time. A plain
sleep is not cut by a signal: 1.8 slept it to its end, and the delay of a login
that a signal could shorten is a delay an attacker chooses.

The wake of a channel loses nothing of its latency, because a wait ends on the
record of the wait and on the door of the port, and not on this loop; what the
loop costs is the continuation of a port sleep that ended early, which is a
call to the port per nudge and not per byte.

### 5. The numbers

`lat_pipe_p50` of x86-64 in QEMU, three runs of `make bench`, the median (the
full table is in `docs/bench.md`):

| Row | 1.8.0 | 1.9.0 | |
|---|---|---|---|
| `lat_pipe_p50` | 1 005 056 ns | **19 968 ns** | a tick → twenty microseconds |
| `lat_pipe_p99` | 1 044 992 ns | **36 352 ns** | the tail moves with the middle |
| `pipe` (4 MiB, both ends) | 31.00 MiB/s | **333-400 MiB/s** | the rate of a pipe that blocks |
| `lat_syscall_p50` | 472 ns | 472 ns | the door of the call does not move |

On the host the same round trip, measured by the case of the suite
(`tests/cases/test_wait.c`, the p50 of 64 bytes, three runs of 4, 4 and 3 µs):
**4 µs**. The wake of a sleeping task, measured from the wake to the stamp of
the waiter: 8-11 µs.

## Consequences

**Good**

* A pipe, a `poll` and the console answer in the time of a switch and not in
  the time of a tick: twenty microseconds against a millisecond, on both
  machines that have a door.
* The rate of a pipe between two programs rises by a factor of six, because
  the writer of a full ring is woken by the reader and no longer waits out the
  rest of a millisecond.
* K15 is closed at its cause. The twenty milliseconds of 1.8 were a bound on
  the damage; the arm under the lock is the reason there is no damage.
* The waiter is a task in the run queue the moment the object changes, which is
  what the many cores wanted: the wake of a channel and the hand-over of a task
  are the same mechanism now, one IPI.

**The price**

* One more critical section around the state of an object (`k_wait_arm`) and
  one flag looked at under the scheduler lock. The arm is a handful of
  instructions under a lock the caller already holds; the flag closes a window
  that no amount of testing could have closed from the other side.
* A wake now touches the run queue of another core from a context that used to
  touch a tick: an interrupt is sent where a number was written. On a machine
  whose quantum is ten milliseconds the interrupt is cheaper than the tick it
  saves.
* The host's sleep is a condition variable per task, and its door must find the
  context of a task that names it: a list of the live contexts and a lock. The
  memory is one pointer and one flag in the block the thread already has.
* The suite had handshakes of `volatile int` between a child and a parent that
  the race detector accepted only because the reader arrived a tick late. The
  wake being immediate made them races in the eyes of ThreadSanitizer, and the
  tests say `atomic_int` now. That is a defect of the tests that the lane
  found, not a change of the kernel.

**What is still open**

* The board keeps the chunk of ten milliseconds: its sleep is `vTaskDelay`, and
  a door over the notification of a FreeRTOS task needs the registry of the
  port to be protected by one lock for both the wake and the end of a task.
  The debt is in `docs/debts.md`, and the port is a compatibility port.
* A woken task of the same priority does not preempt the task that woke it in
  the middle of its slice. It does not need to for the pipe - the writer
  blocks - and a preemption point at the return of a system call is a lane of
  its own, with its own tests of the desk.
* The console of the host reads with `fgets`, so a `poll` of the console is
  still a step of the port and not a channel of the kernel; the x86-64 console
  and its keyboard are the ones that use the channel today.
