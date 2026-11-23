# ADR 0035 - The grain of the I/O: a pipe of the machine, a stream with a block

* Status: accepted
* Date: 2026-11-23

## Context

ADR 0014 made the pipe a ring of 256 bytes with a wait that gives up after
half a second and answers `EAGAIN`, and the C library of a program was left
with no write buffer and a read buffer of 512 bytes inside the object of
every `FILE`. Both were sized for the internal RAM of the board, and both
were too small by two orders of magnitude for the machine that carries them:
a consumer of one megabyte through a pipe waited for the producer about four
thousand times, at a millisecond a wait, and a filter that writes a line at
a time called the kernel once per line. The output of `sed` or `grep` over a
megabyte of text was thousands of calls; a slow reader turned a pipeline into
seconds of sleeping.

The release of the many cores changed the question. The internal RAM of the
board is still scarce, but the block of a pipe need not be the bss of the
kernel and the block of a stream need not be the object of the stream. The
grain is a setting of the port and the memory is paid when it is used.

## Decision

### 1. The ring of a pipe is a setting of the port, allocated with the pipe

`KFILE_PIPE_SIZE` is no longer a constant of the kernel: each build names it,
and the header carries the safe default of the board.

| Machine | Ring | Why |
|---|---|---|
| x86-64, host | 64 KiB | the block of one call of the system (`KSYS_MAX_IO`) |
| ESP32 | 4 KiB | a sixteenth of the internal RAM of a board with a hundred kilobytes |

The buffer is not in `struct k_pipe` any more. `kfile_pipe` takes it from
`kmm_alloc` when the pipe is made and `pipe_end_close` gives it back when the
last end goes; a pipe of 64 KiB costs nothing until it exists, and sixteen
pipes of a machine that has a megabyte of memory cost a megabyte only when
sixteen of them are open. On the board the allocator takes the internal RAM
first and falls back to the PSRAM, and a machine that has no room for the
ring answers `ENOMEM` from `pipe()`: a refusal, not a pipe of another size
that the caller did not ask for.

`kfile_init` gives back the rings of the pipes that were still open when the
tables are reset, so the boot and the tests do not leave a block behind.

The ring is a 32-bit index: 64 KiB does not fit the `k_u16_t` of the old
count.

### 2. The wait has no bound, and a write of PIPE_BUF is one step

The 500 ms bound is gone. A reader of an empty pipe whose writer is there
sleeps until the bytes arrive; a writer of a full pipe sleeps until the
reader makes room. A reader slower than its writer is an ordinary pipeline,
not an error and not a timeout. A signal that has to be answered cuts the
wait short and the answer is `EINTR`, as `waitpid` already did; the bytes of
a write that placed something are the answer, and a reader that placed
nothing answers `EINTR`.

The wait is still a `k_sched_sleep_ms(1)` loop and not a queue of waiters:
the port has no "block the current task and switch" of its own (the
scheduler's block is for a task another core wakes), and the pipeline of the
shell is two or three stages. The queue is the next step and it is a table of
the debts, not a property the release has.

`PIPE_BUF` (512, the number of `<limits.h>`) is the promise of the ring: a
write of at most 512 bytes waits until the whole of it fits and then lands
with the table lock held, so two writers of a pipeline cannot put a byte
inside one block of each other. A write of more than `PIPE_BUF` is one or
more full turns of the ring and is placed as the reader makes room, which is
what POSIX promises of a large write and no more.

`read`, `write`, `EAGAIN`, `EPIPE` and the end of the file keep the meanings
of ADR 0014: a read of an empty pipe with no writer is 0, a write with no
reader is `EPIPE`, and the access of an end is the flag of its descriptor.

### 3. A stream has a block, and it comes from the arena

`setvbuf` is real. A `FILE` has one block, used for the read-ahead and for
the bytes written and not yet given to the kernel; the two directions never
share it at once, which is the rule of an update stream of C.

* The default mode is chosen at the first use: `_IOLBF` for a terminal
  (`isatty`), `_IOFBF` for a file and a pipe, `_IONBF` for the error
  descriptor. 4 KiB is the default block (`ULIBC_STREAM_BLOCK`).
* The memory is the arena of the program (`malloc`, `ULIBC_HEAP_SIZE`), not
  a block inside the object: a stream that never buffers costs nothing, and
  a program with no room for the block keeps working unbuffered.
* `setvbuf(stream, NULL, mode, size)` takes the block from the arena;
  `setvbuf(stream, buffer, mode, size)` uses the block of the caller and
  never frees it. `setvbuf` after I/O flushes the old block and gives the
  read-ahead back to the descriptor when the descriptor can seek.
* `fflush`, `fclose` and the end of the program give the bytes to the kernel.
  `fflush(NULL)` flushes every stream of the program; `exit` flushes them
  after the `atexit` handlers, so the last line of a handler is in the file;
  `_exit` does not flush, as POSIX says.
* `fgetc`, `fgets` and `ungetc` keep their semantics over the block, and a
  read of a block or more goes straight into the caller's memory
  (`fread` does not copy through the block twice). The block of `ungetc`
  stays in the state of the stream, because a flush or a seek gives the
  block back.
* `printf` and `puts` reach the block of `stdout` when the program links
  `src/file.c`, through a weak seam between the formatter and the streams
  (`__ulibc_stream_write_stdout`); a program of the runtime without the
  streams, like every program of the board, writes straight to descriptor 1.

The ABI does not change: no system call is added and no number moves.

### 4. The measurement

`/bin/iobench` (a program of the test, like `mkbig`, not of the image of a
person) and `tools/io_bench.py` are the ruler: one megabyte through a pipe,
one megabyte through `sed` and through `grep`, a stream read a byte at a
time and a stream written a line at a time. The numbers of the two trees
before and after this ADR are in the pull request of the work and in
`docs/debts.md` where a step is still open.

The `splice`-like copy of a file straight into a pipe (`cat file | wc` with
one copy of the bytes) is not in this ADR: it needs a call of the ABI and the
programs of the tree are not written for it. It is a row of the debts with
the shape of the call, under the next free number of the table.

## Consequences

* A pipeline moves its bytes in blocks of 64 KiB on the machine of a desk
  and 4 KiB on the board; a filter writes its output in blocks and not in
  lines.
* The kernel costs a megabyte of memory only when sixteen pipes of the big
  ring are open at once; the board pays 64 KiB of the worst case and 4 KiB
  of a pipeline of two stages.
* A program that reads or writes a `FILE` and dies with `_exit` loses the
  last block, exactly as in every C library; a program that ends with `exit`
  or with a return from `main` does not.
* The wait of a pipe is still a polling sleep: the scheduler is woken every
  millisecond for a task of a pipeline. The queue of waiters is the next
  step, and the accounting of a wakeup is in `docs/debts.md`.
