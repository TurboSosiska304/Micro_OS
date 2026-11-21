# ADR 0014 - Pipes and dup2

* Status: accepted
* Date: 2026-11-21

## Context

A program of this system could open a file and read or write it, and it
could fork and wait. What it could not do is join two programs: there was
no pipe, and there was no way to make a number that already meant
something - the console at 1 - mean a file instead. The gate of 1.0 asks
for `pipe()` and `dup2()` (the release checklist, "Processes and
userland"), because without them a shell cannot redirect output without
the kernel's help.

The descriptor of a task was an entry of the table of the task, and the
open file was the entry itself: a `struct kfile` with the `struct
vfs_file` of the filesystem and the private data of the open file in it.
That shape cannot carry a pipe without a second guess, and it cannot carry
`dup2` at all:

* two descriptors that share one open file must share its position. A copy
  of the entry would give each its own offset, and a `write` through one
  would land on a byte the other had already passed;
* the private data of an open file lives in the entry and `vfs_file.priv`
  points into it. A byte copy of the entry leaves the copy pointing at the
  buffer of the original, which is a use after free the moment the
  original is closed.

The table of the calls ended at 26; `KSYS_COUNT` was 27.

## Decision

### 1. An open file is a counted description

`struct kfile` is now a record of the kernel, and a descriptor is a
reference to it:

```c
struct kfile *files[TASK_MAX_FDS]; /* in struct task */
```

The records live in a fixed table of `KFILE_MAX` (24) entries, like the
tasks and the mounts; an open file is a small record and the board does
not want a heap that fragments for it. A description has a `refs` count.
`dup2` and a fork add a reference; `close` drops one; the last one closes
the open file (VFS close, or the end of a pipe) and gives the record back.

The address of a description is stable while it is open, so
`vfs_file.priv` may point into its own `priv` buffer, as before. The buffer
is aligned for the structures of the filesystems.

The records share one recursive `k_mutex` with the pipes. It is never held
while a task sleeps and never held while the VFS closes an open file: a
close that waits for the flash must not hold the captures of the other
tasks.

### 2. A pipe is a ring of bytes and two counts

```c
struct k_pipe {
    bool used;
    k_u16_t readers; /* the descriptions that hold the reading end */
    k_u16_t writers; /* the descriptions that hold the writing end */
    k_u16_t head;
    k_u16_t count;
    k_u8_t buf[KFILE_PIPE_SIZE];
};
```

A pipe object lives in a fixed table of `KFILE_PIPE_MAX` (8) entries and
carries `KFILE_PIPE_SIZE` (256) bytes. The buffer is small on purpose: the
internal RAM of the board is scarce, and a producer with more to say waits
for the reader. A descriptor of a pipe end has `flags` of `O_RDONLY` or
`O_WRONLY`, so the existing access check at the edge of `read` and `write`
answers `EBADF` for the wrong end without a new rule.

`read` of an empty pipe whose writers are gone is 0, the end of the file.
`write` with no reader is `EPIPE` (POSIX 32), the bytes before it are the
answer and there is no signal, because this kernel has no signals yet. The
counts are of descriptions, not of descriptors: a `dup2` of an end does
not close it, and neither does a fork.

The wait of a pipe is a poll with a bound, not a queue of waiters.
`KFILE_PIPE_WAIT_MS` (500 ms) of `k_sched_sleep_ms(1)` is the most a call
waits; after that a read of an empty pipe with a writer is `EAGAIN`, not 0,
and a write that placed no byte is `EAGAIN` too. A read or a write that
placed something returns the count it placed. A queue of waiters and a
wakeup is the next step; the bound is what keeps a program whose other end
is gone from being stuck for ever.

### 3. The ABI: two calls at the end of the table

```c
#define KSYS_PIPE 27
#define KSYS_DUP2 28
#define KSYS_COUNT 29
```

`pipe(fds)` needs the place for two `int`s in the memory of the caller:
NULL is `EINVAL`, a place that is not the caller's is `EFAULT` (checked
with `k_port_exec_contains`), and the answer is 0 with `fds[0]` reading
and `fds[1]` writing. The two descriptors are ordinary ones - the first
free slots of the task - so a program that has closed its console gets 0
and 1, which is what a shell wants.

`dup2(oldfd, newfd)` answers `newfd`. A number outside the table is
`EBADF`, a descriptor that is not open is `EBADF`, and `oldfd == newfd`
changes nothing. The descriptor at `newfd`, if it was open, is closed by
the call; the old one keeps a reference to the same description.

The library has `microos_pipe()` and `microos_dup2()`, and `pipe`/`dup2`
when `libc/unistd.h` is included, like the rest of the calls. They answer
-1 and set `microos_errno()`.

### 4. Descriptors across fork and exit

A fork copies the table of the parent: every open file of the parent gets
one more reference before the child can run - the port starts the child
only after the copy. That holds for the real copy-on-write fork of x86-64
and for the placeholder of a machine without an MMU, which now has its own
spawn helper (`task_fork_shared`) so that the copy is not a race. A task
that exits closes its descriptors before it is published as a zombie, so a
parent never reaps a slot that still holds a record of the file table.

A descriptor that a parent and a child share points at one description:
the end of a pipe is closed by the last descriptor of both, exactly as
POSIX leaves it.

## Consequences

**Good**

* A program can make a pipe and send its output to a file or to another
  program; the shell has the two calls it was missing.
* One open file has one position, however many descriptors name it, which
  is the POSIX rule a program of 1.0 expects.
* The table of the calls only grew at the end: 0-26 keep their numbers and
  `statx` stays the last of the old block.
* A pipe end is an ordinary descriptor: the access checks, `close`, `dup2`
  and the fork need no special case at the edge.
* The static RAM of the descriptors went down, not up: the record moved out
  of `struct task`, so a fixed table of 24 descriptions and 8 pipes takes
  less than the four descriptions of each of the 32 tasks did.

**Bad**

* Eight pipes of 256 bytes and 24 open files are system-wide limits. They
  are enough for the shell and a chain of a few commands, and they are not
  a number a program can ask about or raise.
* A blocked call polls once a millisecond and gives up after half a second;
  a real wait queue is the next step, and until then a pipe is not the
  place for a slow producer of more than the bound.
* There is no `SIGPIPE` and no `poll`/`select`: a program learns about a
  closed reader by the error of `write` and cannot watch two descriptors at
  once.
* There is no shell pipeline yet. The kernel side of `cmd | cmd` is here;
  the parser, the redirects and the wait of a chain are the next step.
* Fork copies every descriptor; there is no close-on-exec flag, so a
  program started by `execve` inherits what its caller had open.

## Files

`components/kernel/include/kernel/kfile.h`,
`components/kernel/src/kfile.c`, `components/kernel/src/ksys.c`,
`components/kernel/src/task.c`, `components/libc/src/unistd.c`,
`docs/syscall-abi.md`, `tests/cases/test_ksys.c`,
`tests/cases/test_ksys_process.c`, `tests/cases/test_libc.c`.
