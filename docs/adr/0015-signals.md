# ADR 0015 - Signals and the terminal

* Status: accepted
* Date: 2026-11-21

## Context

The gate of 1.0 asks for basic signals: `SIGINT`, `SIGTERM`, `SIGCHLD`, a
`waitpid()` that a signal can interrupt, and either job control or an
explicit "there is none" in the shell. The kernel already had processes on
the x86-64 machine - a `fork()` with page tables of its own, `execve`,
`exit` and `waitpid` - and no way for one task to ask another to stop. The
shell could throw away a line with Ctrl+C, and nothing else.

Two properties of the tree shaped the answer. A program is a task of the
scheduler, and on the x86-64 port it is a program of ring 3 with page tables
of its own; the kernel cannot call a function of it as if it were a kernel
function. And the existing `waitpid()` sleeps one millisecond at a time for
up to five seconds, which is a natural place to notice a mark set by
another task.

The ABI is frozen: the calls 0-26 keep their numbers, and the next free
number is 27.

## Decision

### 1. A mark, a disposition, and a point of delivery

A signal is a bit in `struct task` (`sig_pending`) and a handler in a small
table of the same task (`sig_handlers[TASK_SIGNALS]`). The table has eight
places and four are used: `SIGINT` (2), `SIGPIPE` (13), `SIGTERM` (15) and
`SIGCHLD` (17). `SIGPIPE` is in the contract although there is no `pipe()`
yet, because the next door of the ABI is a pipe and a program that writes
one has to die of the same signal everywhere.

The dispositions are the two values of POSIX, `SIG_DFL` (0) and `SIG_IGN`
(1), or an address of the task. The default action of `SIGINT`, `SIGTERM`
and `SIGPIPE` is the end of the task with the status `128 + the number of
the signal`; the default of `SIGCHLD` is to be ignored. A handler installed
by a program is memory of that program: `signal()` checks the address with
`k_port_exec_contains()` and answers `EFAULT` for a pointer that is not the
window of the caller.

Delivery happens in the context of the task that receives the signal, at
the two points where the kernel already is:

* on the way out of a system call, in `ksys_call()` after the handler of the
  call has returned and before the result reaches the program;
* on the tick of the timer, when the interrupted code is ring 3.

Nothing is delivered from the task that sends the signal: the sender only
sets a bit (under the critical section), wakes a blocked task and returns.
A task that never enters the kernel and never takes a tick cannot be
interrupted; every program of this system gets the tick.

### 2. The frame of a handler on x86-64

The x86-64 port builds a signal frame on the stack of the program below the
current `RSP`: the registers and the five words of the `iretq` of the
moment, with a magic that says the bytes are a frame of the kernel. The
handler is entered by rewriting three words of the saved frame - `RIP` to
the handler, `RSP` to the frame, `RDI` to the number of the signal - and the
`iretq` of the stub does the rest. The handler is an ordinary function of
the program; it runs in ring 3 and returns with `ret`.

The return address on that stack is a page of the port, mapped into every
program at a fixed address below the guard page of the stack:
`mov $KSYS_SIGRETURN, %eax; int $0x80`. The call `sigreturn` (29) is not
for the program: it restores the saved frame and answers with the value the
interrupted context had, which may be a negative answer of the call the
handler interrupted. The frame is a `struct port_signal_frame` of the port,
not a structure of the ABI: a program never sees it and never builds one.
The selectors and the flags of the restored `iretq` come from the sigreturn
call itself and not from the saved copy: the frame is memory a program can
write, and an `iretq` that trusted a selector or an IOPL from it would be a
door into ring 0 or to the ports of the machine.

The host, the board and the null port have no ring 3, so the kernel runs the
handler in place, in the task's own context. That is also what a kernel task
of any port gets when it installs a handler.

The two calls of the task substrate, `k_port_task_signal_enter()` and
`k_port_task_signal_leave()`, are additions to the contract of the port, so
its version moves from 1.6 to 1.7: a port of 1.6 lacks them, and the boot
name of the pair says so instead of failing in the middle of a signal.

### 3. The calls

The three calls take the next free numbers:

| Number | Call | Arguments | Result |
|---|---|---|---|
| 27 | `kill` | `pid, sig` | 0, or an error |
| 28 | `signal` | `sig, handler` | the previous disposition, or an error |
| 29 | `sigreturn` | - | the saved value of the interrupted call |

`kill` accepts a positive pid only: the system has no process groups, so
the meanings of POSIX for 0 and -1 ("the group" and "everybody") are not
guessed at and are `EINVAL`. A pid that names no live task is `ESRCH`; the
number is the shared `-3` of the errno table (`KERR_SRCH` is an alias of
`KERR_EXIST`), because `-3` became "file exists" before signals and the
frozen table cannot be renamed. The shell prints the sentence of the
signal, not the generic text of `-3`.

`signal` keeps the handler installed until the task changes it (the shape
of `signal()` of Linux, not the SysV reset), and setting `SIG_IGN` throws a
pending mark of that signal away. A fork copies the dispositions and not
the marks: the marks belong to the moment of the parent.

### 4. The wait, and the child

`waitpid()` looks for a finished child first and only then at the marks: a
parent whose child has just ended gets its status even though the `SIGCHLD`
of that very child is pending. When there is a pending signal that has to
be answered - the default end of the task, or a handler the task installed
- the call answers `EINTR` without reaping the child, and the handler runs
on the way out of the call. `ECHILD` keeps its meaning: a pid that is not a
child of the caller, or a wait that found no child at all.

The end of a task marks `SIGCHLD` on its parent before the slot becomes a
zombie; a task without a parent leaves the mark nowhere. A sleeping task is
woken by the tick when a pending signal has to be answered, so a `sleep` of
a second does not delay the handler of a signal that arrived in the first
millisecond.

### 5. Ctrl+C and job control

There is no job control: no groups, no `&`, no `fg`, no `bg`, and the shell
says so in `help kill` and in the wiki. What the system has is the one
signal a person needs at the console: while the shell waits for the program
it started (`exec`), the port polls the console in the same loop, and a
Ctrl+C byte sends `SIGINT` to that program. A key that is not Ctrl+C is
dropped: a running program does not read a line, and there is no typeahead.
At the prompt the Ctrl+C of the shell still throws the line away. `kill`
takes the pid from `ps` and sends the signal to it.

## Consequences

**Good**

* A program has a handler that runs in user space, a default action that
  ends it, an ignored signal, and a wait that a signal cuts with `EINTR`.
* The kernel never calls a function of a program from its own context on a
  machine with ring 3, and a broken handler ends its task and not the
  machine.
* `SIGCHLD` follows the exit of a child without polling, and the numbers
  0-26 are untouched.

**Bad**

* There are four signals and no groups. There is no `SIGKILL` that cannot be
  caught, no `SIGSTOP`, and no way to signal a whole job; `kill -9` is a
  number outside the table and is refused with `EINVAL`.
* A task of the kernel that never makes a system call is not checked on the
  host; only the programs of x86-64 are checked by the tick as well.
* The return from a handler is the `sigreturn` of a page the port owns; a
  program that overwrites that page breaks its own handlers, and the frame
  of a signal and the frame of an `iretq` are two shapes a port has to keep
  in step (they are written down in `port_exec.c`).
* Ctrl+C is a byte of the console and not a job-control character: it
  reaches only the program the shell is waiting for, and a program that
  ignores `SIGINT` cannot be stopped from the keyboard.
