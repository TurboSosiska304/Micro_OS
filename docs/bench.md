# The numbers of the machine

A release is compared with the one before it by a table of numbers and not by
an impression, and the table has to be the same table every time. `make bench`
is that table: three boots of the x86-64 port, a fixed set of work, and one
rate (or one latency) for each piece of the kernel the work leans on. A run of
one boot is one sample and one sample is not a number of a release, so every
row of the table is the median of the runs.

The program that measures is `ports/port_x86_64/programs/bench.c`; the tool
that writes the images, boots QEMU and reads the lines back is
`tools/x86_bench.py`. The program is **not** in the image of the release: the
tool puts it there for the run, the way `tools/x86_volume_smoke.py` puts
`/bin/mkbig`, because a measurement is a tool of the gate and not a command of
the machine.

## The run

```bash
make bench                                  # three boots, the median of each row
make bench BENCH_ARGS="--runs 1"            # one boot, for a look
make bench BENCH_ARGS="--json bench.json"   # the same table as JSON as well
make bench BENCH_ARGS="--fs ext2"           # the same tree on an ext2 root
make bench BENCH_ARGS="--fs ext2 --block-size 4096"   # the grain of the volume
make bench BENCH_ARGS="--verbose"           # the console of every line
```

The tool boots the machine `--runs` times (three by default) and every run gets
a volume of its own, written into a directory under `build-x86/` that it
removes when the runs end (`--keep` or `--dir` keep them; with `--dir` the
image of run N is `bench-<fs>-<N>.img`). A fresh volume per run is not a
convenience: the program writes a file of 16 MiB into it and the place of that
write in the volume is part of the number of `file_write` (see "Reading a
number").

Three runs are about half a minute of emulation and are **not** a part of
`make ci`: it is a release gate, like `make x86-volume-smoke`, and not a check
of a push. `make bench` builds what it needs (`x86-build`, `build`).

The sizes are constants of `tools/x86_bench.py` (`PARAMETERS`), passed to the
program on the command line; the program names them in its own `main` and does
not choose. A release that changes a size does not change the meaning of a
row: it adds a row.

| Row | Size | The line |
|---|---|---|
| `syscall_getpid` | 100 000 calls | `exec /bin/bench syscall 100000` |
| `fork_exec` | 20 runs | `exec /bin/bench fork 20` |
| `pipe` | 4 MiB (4 194 304 B) | `exec /bin/bench pipe 4194304` |
| `file_write` | 16 MiB in 64 KiB blocks | `exec /bin/bench filewrite /bench.big 16` |
| `file_read` | the same file | `exec /bin/bench fileread /bench.big 16` |
| `console` | 200 lines of 43 bytes | `exec /bin/bench console 200` |
| `screen_text` | 8 000 cells (160x50) | `exec /bin/bench screen 8000` |
| `lat_syscall_p50`, `lat_syscall_p99` | 2 000 calls | `exec /bin/bench latsyscall 2000` |
| `lat_pipe_p50`, `lat_pipe_p99` | 256 bytes | `exec /bin/bench latpipe 256` |
| `shell_pipe` | 4 MiB | `cat /bench.bin \| wc -c` |

## The table

Every row is one line of the console, and the shape of the line is the
contract between the tool and the reader of it:

```text
bench: <name> <median> <unit> (median of 3)
```

The value has two decimals and the unit is one of `calls/s`, `runs/s`,
`MiB/s`, `lines/s` and `ns`. The line carries the median of the runs, which is
the number two releases are compared by; a table of one boot prints `(run)`
instead and is a look, not a number of a release. The value of each run is in
the human summary (`results[].values` in the JSON) and the spread of the runs
is beside the median.

The program prints a `bench note: <name> <count> <what> in <ms> ms` line beside
every row with the bytes and the milliseconds behind the rate; the tool
ignores it and prints it in the human summary. The two latency measurements
add notes of their own — the floor of the counter and the samples that fell
beyond the histogram — and the tool ignores those as well. A failure of a
benchmark is a line `bench error: ...` and a status of the program that is not
zero, which is the answer of the tool as well.

Eleven rows are the clock of the machine (the program measures itself) and one,
`shell_pipe`, is the wall clock of the host, because the shell pipeline prints
no rate of its own. The tool prints the seconds of the host beside every row
of the human summary so that the two clocks are never confused.

## The latency of one call

The rates are many operations in one clock; the two latency rows are one
operation at a time, and they answer the question a rate cannot: what does
*one* call cost, and how bad is the worst one in a hundred.

The clock of the machine ticks a millisecond, which is a thousand calls of the
door, so the program reads the time stamp counter of the processor around the
call itself. The counter is calibrated in the program against the millisecond
clock over a window of 100 ms (a program has no other clock to borrow: the
kernel knows the frequency of the counter and a program cannot ask it), and
QEMU advances the counter with the clock of the emulated machine, so a
nanosecond of these rows is the same nanosecond as the milliseconds of the
others.

Every sample is one pair of reads of the counter around one operation, and the
pair is not free: the row includes it. The program measures the same pair
around nothing, in the same loop, and prints the floor beside the histogram
(`bench note: lat_syscall_floor <ns> ns`); on the machine below the floor is
12 ns against a call of 472 ns, so the row is the call and the floor is a
fortieth of it.

* `lat_syscall` is one call of `getpid()` at a time. The rate row says how many
  of them fit in a second; this row says what one of them costs and how long
  the slowest one in a hundred is.
* `lat_pipe` is one message through a pipe at a time. The writer stamps the
  counter just before the write, the reader stamps it just after the read and
  sends the stamp back through a second pipe, and what the writer records is
  the age of the byte, the wake of the reader included. One message is in
  flight at a time — the writer waits for the answer before it stamps the next
  byte — so no sample waits in a queue of its own.

The pipe row reads the scheduler and not the copy, and it was worth a second
look: from 1.5 to 1.8 the row was **about 1 ms, one tick of the timer.** A
blocked reader was woken by the write in the kernel, but a wake only moved the
tick of the sleeper (`components/kernel/src/wait.c`: a waiter asleep was woken
by the scheduler at its next tick), so the read returned about one tick later
and the state of the task, not the copy of the byte, was the number. The rest
of the row is the same: its two ends retry `EAGAIN`, and 4 MiB of 256-byte
handoffs at 31 MiB/s is 8 µs for a handoff. The two numbers never disagreed;
they measured a busy pipe against an empty one, and the wake that had to wait
for the tick was what a program that blocks on a pipe paid.

1.9 moved that number and this row with it (ADR 0046): the writer puts the task
of the reader back in the run queue in the call of the write, and the sleep of
the port is cut, so the middle byte of a pipe is **twenty microseconds** and
not a millisecond. The table of the change is below, "The wake of 1.9".

The percentile is the nearest rank — the sample at the ceil(p % of the count)th
place — and the value of a bucket is its middle, which is the resolution of the
row: 16 ns for the call (a range of 65 µs) and 1 µs for the pipe (a range of
4 ms). A sample beyond the range is counted in the last bucket and the program
says how many there were (`bench note: lat_pipe_beyond ...`): a row that is a
floor of the truth does not hide it.

The rows of the machine of this document, the median of three runs (a second
gate of the same tree answered the same 472 ns and 1 006 080 ns for the two
p50 rows, and 536 ns against 664 for the p99 of the call):

| Row | Three runs | Median | Spread |
|---|---|---|---|
| `lat_syscall_p50` | 472, 472, 472 ns | **472 ns** | 0 % |
| `lat_syscall_p99` | 536, 664, 920 ns | 664 ns | 58 % |
| `lat_pipe_p50` | 1 004 032, 1 006 080, 1 007 104 ns | **1 006 080 ns** | 0.3 % |
| `lat_pipe_p99` | 1 027 584, 1 033 728, 1 035 776 ns | 1 033 728 ns | 0.8 % |

The call of the door is 472 ns in every run, which is 0.47 µs and agrees with
the rate of `syscall_getpid` (about 2.0 million calls a second): a row that is
made of many calls and a row of one call are two readings of the same cost,
and the first release that moves one should move the other.

The p99 of the call is the row with the widest spread of the whole table, and
it is reported and not gated: the tail of 2 000 calls is the moment the host
scheduled something else, so 536 ns and 920 ns are the same tree on a shared
machine and not two trees. `lat_syscall_p50` is the row of the gate; a lane
that changes the door looks at both.

The pipe's p99 was the other way around until 1.9: the tick was a hard floor
under the handoff and the tail was 3 % of the middle, so the two rows moved
together. With the wake of ADR 0046 the middle is twenty microseconds and the
tail is still twice the middle - the second wake of the round trip and the
switch of the emulator - and the two rows still move together.

## The wake of 1.9 (ADR 0046)

The two latency rows of the pipe and the rate of the pipe were one shape from
1.5 to 1.8: a waiter was woken by moving its tick to the present, and the timer
did the rest, so an empty pipe answered in a millisecond. ADR 0046 puts the
waiter in the run queue of its core in the call of the writer (the IPI of
ADR 0034 when it is another core), and cuts the sleep of the port, so the same
byte costs the two handoffs of the two tasks.

The table is the same tool on the same machine (AMD Ryzen 5 5600, QEMU 11.1.3,
TCG, 256 MB, one processor), the tree of 1.8.0 (`a8096c5b`) against this one:

| Row | 1.8.0 | 1.9.0 | Change |
|---|---|---|---|
| `lat_pipe_p50` | 1 005 056 ns | **19 968 ns** | 50 times |
| `lat_pipe_p99` | 1 044 992 ns | **36 352 ns** | 29 times |
| `pipe` (4 MiB, both ends) | 31.00 MiB/s | **333.33-400.00 MiB/s** | 11-13 times |
| `lat_syscall_p50` | 472 ns | 472 ns | the door does not move |

The 1.8 column is three runs of `--runs 1` (1 003 008, 1 005 056 and
1 009 152 ns for the p50) and agrees with the table of the release above
(1 004 032, 1 006 080, 1 007 104 ns). The 1.9 column is one `make bench` of
three boots, and the middle byte was steady enough to land in one bucket:
19 968, 19 968 and 19 968 ns, spread 0.0 %, and the p99 of the same boots
35 328, 36 352 and 43 520 ns. A second batch answered 18 944, 19 968 and
19 968 ns for the p50 and 46 592 ns for the p99, and a `pipe` of 333.33 MiB/s
against the 400.00 of this one: the rate of the pipe is the row that moves with
the load of the host, and its three runs of this batch were 363.63, 400.00 and
400.00 MiB/s.

The suite measures the same round trip on the host port
(`tests/cases/test_wait.c`, 64 bytes, the p50 of each run printed as `wait: the
p50 of a byte through a pipe is <n> us`): **4 µs**, three runs of 4, 4 and
3 µs, and the wake of a sleeping task from the wake to the stamp of the waiter
is **8-11 µs** (10, 8 and 11). The
host is not in the table above because it has no `/bin/bench`: the case of the
suite is its number, and it is the same mechanism.

Two things about the 1.8 side of this table are worth a note of their own. The
tool of 1.8 hung twice on this host while the numbers above were taken: once
after `pipe`, once after `console`, both times on a step of `/bin/bench` whose
child never answered and the shell waited for it - the lost wakeup of K15 as a
timeout of 900 seconds. The bound of twenty milliseconds of 1.8 made that a
rare shape and not an impossible one; the arm under the lock of the object is
what removes it, and the case of the window in the suite is the proof.

The sleep of the kernel was the one shape of the lane that a gate found after
the numbers above: a sleep of `ms` could end at once when the nudge of another
sleep arrived for it, and `k_sched_sleep_ms()` is a loop on the tick of its
deadline now (ADR 0046 §4). One boot of the same tool after that change
answered **20 992 ns** for `lat_pipe_p50` and 41 472 ns for the p99: the loop
did not move the row, as it should not - a wait ends on the state of the task
and not on the sleep that carried it.

The port of the board is not in this table: its sleep is `vTaskDelay`, which a
task of the kernel cannot cut, so its waits keep the ten milliseconds of
`WAIT_CHUNK_MS` (ADR 0046, "What is still open", and `docs/debts.md`). A
machine of a hundred kilobytes of SRAM has one scheduler and it is not the one
of this kernel.

## The gate of a release

The gate of a release is **three runs and the median of each row**: `make
bench` with no arguments. A row that moves by less than a tenth between a
release and the one before it is not a signal; a row that moves by more is a
claim that has to survive the spread of the runs.

Seven rows are the gate of a release, and a release does not pass while one of
them is missing or zero. (The tool refuses a table with any row missing; these
are the ones the gate is read by.)

| Required row | What it answers |
|---|---|
| `syscall_getpid` | the door of the system call and its cost |
| `fork_exec` | a task, an image and the wait of a parent |
| `pipe` | the handoff of the pipe and the scheduler behind it |
| `file_write` | the allocator and the writer of the filesystem |
| `file_read` | the reader and the cache of the host |
| `screen_text` | one full screen through the console and framebuffer |
| `lat_syscall_p50` | the middle call of the door, in nanoseconds |

The other rows are compared as well, and a release that loses one of them has
a shorter table, which is a decision of the lane and not of the run.

Two warnings go with the median, and neither one fails the tool by itself,
because both are facts of the host and not of the tree:

* **the spread of the runs.** A row whose runs are more than a tenth apart is
  printed below the table as `the runs disagree: <row> spread <n> %` and the
  JSON carries the same number in `"spread"`. That row is the load of the host
  of the day and not the tree; `file_read` is the one that does it most, with
  the cache of the host under it.
* **a starved run.** The tool compares the clock of the guest with the wall
  clock of the line (see the end of this file) and a run that was starved
  **leaves the median**: the table is the median of the clean runs and the
  tool says which run it dropped. When every run was starved there is no table
  to report and the tool fails.

The JSON of the gate has the same facts as fields: `"median"` (the value of
each row), `"runs"` (the runs in the median), `"spread"` (the percent of each
row), `"starved"` (the rows of a dropped run) and `"starved_runs"` (the
numbers of those runs), with every per-run value in `results[].values`.

## The second disk of 1.6 (ADR 0037)

The disk of the x86-64 port was the drive of the IDE channel; 1.6 adds the
virtio-blk device of the same image, and the row of a file is the same on both
because the disk is 1 % of the time of a filesystem. One row moved, and it
moved for both of them: the block layer learnt to ask a device that moves a
run for the run (`struct k_blockdev::read_ahead`, 128 sectors of the cache of
each disk), and the read of a file that is in the cache of the host went from
134 to 262 MiB/s.

| Row | 1.6.0 (IDE) | IDE, after | virtio-blk |
|---|---|---|---|
| `file_write` (MicroFS, 16 MiB) | 5.21 MiB/s | 5.39 MiB/s | 5.37 MiB/s |
| `file_read` (MicroFS, 16 MiB) | 134.45 MiB/s | **262.29 MiB/s** | **246.15 MiB/s** |
| `file_write` (ext2, 16 MiB) | 15.29 MiB/s | 14.91 MiB/s | 15.16 MiB/s |
| `file_read` (ext2, 16 MiB) | 51.11 MiB/s | 62.50 MiB/s | 59.47 MiB/s |

The volume of 5 GiB (`make x86-volume-smoke`), the same two disks:

| What | 1.6.0 (IDE) | IDE, after | virtio-blk |
|---|---|---|---|
| ext2, write 128 MiB | 13.96 MiB/s | 13.34-13.91 MiB/s | 13.91 MiB/s |
| ext2, read 128 MiB | 35.17 MiB/s | 36.90-40.18 MiB/s | 40.92 MiB/s |
| MicroFS, write 16 MiB | 6.43 MiB/s | 6.31-6.47 MiB/s | 6.42-6.49 MiB/s |
| MicroFS, read 16 MiB | 74.07 MiB/s | 89.89-105.26 MiB/s | 101.27-105.96 MiB/s |

The two disks of one row differ by the noise of the host and the run-to-run
spread of the row (a tenth, and a quarter for `file_read` of the cache of the
host); the two columns of "after" are one tree with two devices. The ceiling of
a file written to ext2 is the sector operations of the block layer and of the
filesystem - about five per kilobyte written and nine per kilobyte read - and
no device of this machine changes that number: it is `docs/debts.md` under the
grain of the disk, with the plan of the 4 KiB sector.

`make bench BENCH_ARGS="--disk virtio"` is the table of the other disk and
`make bench BENCH_ARGS="--fs ext2 --disk virtio"` the other filesystem on it;
`make x86-volume-smoke X86_DISK_BUS=virtio` is the gate of the large volume on
the card.

## The baseline of 1.5.0

The numbers below are the tree of 1.5.0 (`release_candidate` at `07cab2d`,
`KERNEL_VERSION` 1.5.0) with this tool and no other change. They are the
reference the two lanes of the line of 1.6 are compared by: `iogrind` (a pipe
of 256 bytes and the unbuffered streams of the library) and `fastsys` (the
entry of a system call through the IDT). When a lane lands, `make bench` runs
again and the rows that moved are the answer.

```text
bench: syscall_getpid 1282051.28 calls/s
bench: fork_exec 322.58 runs/s
bench: pipe 0.20 MiB/s
bench: file_write 6.63 MiB/s
bench: file_read 126.98 MiB/s
bench: console 1010.10 lines/s
bench: shell_pipe 0.17 MiB/s
```

| Row | Value | Unit | Spread of four runs | Guest time | Host time |
|---|---|---|---|---|---|
| `syscall_getpid` | 1 282 051.28 | calls/s | 1.19–1.28 M | 78 ms | 106 ms |
| `fork_exec` | 322.58 | runs/s | 317–328 | 62 ms | 97 ms |
| `pipe` | 0.20 | MiB/s | 0.20 | 19 712 ms | 19 741 ms |
| `file_write` | 6.63 | MiB/s | 6.3–6.7 | 2 412 ms | 2 466 ms |
| `file_read` | 126.98 | MiB/s | 101–133 | 126 ms | 158 ms |
| `console` | 1 010.10 | lines/s | 990–1 026 | 198 ms | 235 ms |
| `shell_pipe` | 0.17 | MiB/s | 0.17 | — | 23 437 ms |

The spread is four runs of the same tree, one of them a build from an empty
`build-x86/`: `pipe` and `shell_pipe` did not move at all, `fork_exec` and
`console` moved by a few percent, and `file_read` moved by a quarter, which
follows the load of the host and not the tree. **A difference of a tenth is
not a signal**; a lane that claims a rate runs the tool three times and
compares the middle of the three.

The machine of the measurement: QEMU 11.1.3 of the host, TCG (no `-accel`),
256 MB, one processor, the default `pc` machine, the disk on the legacy IDE
channel with `cache=writeback`; the host is an AMD Ryzen 5 5600. The boot of
the machine is 0.3 s and the run is 47.5 s, of which 43 s is the two pipe
measurements.

The rest of the human summary of that run:

```text
  boot 0.3 s, the run 47.5 s, the image 64 MB of mfs
  name                      value unit           guest      host  what
  ---------------- -------------- --------- ---------- ---------  ----------------------------------
  syscall_getpid       1282051.28 calls/s        78 ms   106 ms  getpid() in a loop
  fork_exec                322.58 runs/s         62 ms    97 ms  fork + execve /bin/echo + waitpid
  pipe                       0.20 MiB/s       19712 ms 19741 ms  4 MiB through one pipe, both ends
  file_write                 6.63 MiB/s        2412 ms  2466 ms  16 MiB written in 64 KiB blocks
  file_read                126.98 MiB/s         126 ms   158 ms  16 MiB read in 64 KiB blocks
  console                 1010.10 lines/s       198 ms   235 ms  200 lines written to the console
  shell_pipe                 0.17 MiB/s              - 23437 ms  cat /bench.bin | wc -c (wall clock)
```

## The volume of ext2 and the grain of its blocks (ADR 0037, ADR 0043)

`--block-size 1024|2048|4096` is the grain of the ext2 volume (the default is
1024, the block of the image of the release). The same table at the two grains
of ADR 0043, three runs each, on the machine of the lane of 1.8:

| Grain | `file_write` | `file_read` |
|---|---|---|
| 1024 bytes | 285.71 MiB/s | 444.44 MiB/s |
| 4096 bytes | 186.04 MiB/s | 363.63 MiB/s |

The coarse grain is the slower one on the disk of the port: the cost of a
write is the command of the channel and not the byte, and the metadata of a
volume of 4 KiB blocks is four times the bytes for the same record. ADR 0043
names the numbers of the volume of 5 GiB and the reason.

## The volume of ext2 (ADR 0037)

The table above is the volume of MicroFS, which is the default of the tool.
`--fs ext2` writes the same tree into a volume of ext2 and the same file of
16 MiB goes through it; the rows that move are `file_write` and `file_read`,
and the lane of ADR 0037 moved them (the tree of 1.6.0 before the lane, the
same machine, three runs each):

| Row | Before ADR 0037 | After | Guest time before | After |
|---|---|---|---|---|
| `file_write` | 15.05 MiB/s | **246.15 MiB/s** | 1 063 ms | 65 ms |
| `file_read` | 50.95 MiB/s | **200.00 MiB/s** | 314 ms | 80 ms |

The write is a run of 64 KiB of the medium for every 64 KiB of the file: the
descriptor and the bitmap of the group are a slot of the volume that settles
at the end of the run, and the blocks of the run are allocated, named and
written once. The read is a run of the blocks that the file holds one after
another, found by one read of the leaf table of pointers, and a hold of holes
is one `memset`. The other rows of the table do not move: the pipe, the
console and the call of the door do not touch the disk. The spread of the
runs is 225-262 MiB/s written and 195-200 MiB/s read, on the machine of the
baseline and with the load of the host of the day; the number in the table is
the middle of the three.

The file of 128 MiB of `make x86-volume-smoke` (a volume of 5 GiB) is written
at 132-147 MiB/s and read at 80-88 MiB/s by `/bin/mkbig verify`, whose
comparison of every byte is part of its clock; the same file read by `bench
fileread` is 185 MiB/s. The reader of a stream is the run of eight sectors of
`K_BLOCKDEV_READ_AHEAD` in the block layer (`kernel/blockdev.h`), so a file is
a command of the disk every 8 KiB; that is a decision of ADR 0032 and the
next step of the reader, not of this lane.

## Reading a number

Two shapes of the kernel decide most of the table, and both of them are the
subject of a lane of 1.6:

* **The pipe is a ring of 64 KiB** on x86-64 and the host, 4 KiB on the board
  (`KFILE_PIPE_SIZE`, ADR 0035), and a write waits when the ring is full
  instead of answering `EAGAIN`. The 4 MiB of `pipe` is therefore a writer and
  a reader that both block, and its rate is the scheduler's and not the
  copy's: 31 MiB/s in 1.8 was 64 KiB per two milliseconds, and the 333-400
  MiB/s of 1.9 is the same handoff without the tick (ADR 0046, "The wake of
  1.9"). `shell_pipe` pays the same shape through the library of `cat` and
  `wc`: 2.4 MiB/s is a line at a time and a program started per stage, not the
  ring.
* **A system call enters through the IDT** (`int $0x80`). `syscall_getpid`
  is that door and a call of no work: 1.28 million calls a second is 0.78 µs
  of the emulated processor per call, and a release that enters through a
  faster gate moves the row.

The other rows have the answers of the code they lean on:

* `fork_exec` is the copy-on-write fork of a program, the image of
  `/bin/echo` (28 KiB) read from the volume and started, and the wait of the
  parent: 3.1 ms per run of the emulated machine.
* `file_write` is the filesystem and not the disk: the volume is MicroFS, a
  file grows block by block with the map and the record of each block, and
  the rate (6.6 MiB/s) is the allocation. The row is a write of 16 MiB to a
  volume that holds a file of 4 MiB and has about 55 MiB free; a second file
  of 16 MiB on the same volume writes at 3.7 MiB/s, because the allocation
  scans the map from its first bit on every block (the debt in `debts.md`).
  The position of the write in the run is part of the number, which is why
  the order of the lines is fixed.
* `file_read` is 20 times faster than the write because a read is a chain of
  blocks and a copy, and because the image of the machine is a file of the
  host: QEMU reads it through the page cache of the host. A second read of
  the same file and a read after another file of 16 MiB evicted the port's
  cache gave 121–124 MiB/s, so the row is the read path and not a cache of
  the port (which holds half a megabyte). The warm page cache of the host is
  under every release, which is what makes the row comparable; it is not the
  rate of a disk of a machine.
* `console` is one `write` of 43 bytes per line to the serial console: the
  door and the emulated UART, and not the buffered `printf` of the library
  and not the shell's `echo` (which adds a program start per line).
* `screen_text` is one write of 8 000 printable cells after a clear, timed by
  the TSC around the whole write. It is the console, the 8x16 glyphs and the
  framebuffer of one 160x50 screen, not the serial rate of `console`.

## The framebuffer of 2.0.2

The defect was measured on **26 November 2026** on an AMD Ryzen 5 5600 host
(AuthenticAMD, 12 logical CPUs), QEMU 11.1.3 TCG, one emulated processor,
256 MB and `-vga std`. The before tree is `03a3d869` (the TSC probes before the
cache and drawing changes); the after tree is `8c1501ac`. The benchmark and
compositor measurement launches waited until no other `qemu-system` process
was running.

| Work | Before | After | Change | Spread before / after |
|---|---:|---:|---:|---:|
| `screen_text` | **27.288787 ms** | **27.411804 ms** | +0.45 % | 1.18 % / 2.21 % |
| `wm` first frame | **5.565531 ms** | **5.857697 ms** | +5.25 % | 54.15 % / 47.81 % |

`screen_text` is the median of three `make bench` runs. Its runs were
27 272 336, 27 288 787 and 27 595 228 ns before, and 27 744 985, 27 138 128 and
27 411 804 ns after. The write is exactly 8 000 cells after a clear, with the
TSC pair around the write; the calibration and the final newline are outside
the interval.

The `wm` row is the median of seven boots of the shell-started desktop on each
side. The first full frame (wallpaper, panel and pointer, before the windows)
is timed by TSC. Before: 5 512 635, 5 555 827, 5 558 533, 5 565 531, 5 828 795,
5 891 803 and 8 526 266 ns. After: 5 613 390, 5 628 112, 5 652 283, 5 857 697,
8 058 639, 8 186 520 and 8 413 897 ns. Both sides are bimodal: most frames are
5.5-5.9 ms and a few are 8.1-8.5 ms. The spread is wider than the 5.25 %
difference, so the table does **not** claim a compositor speedup or regression.

That is the expected QEMU result: its video memory is ordinary RAM and TCG
does not turn PAT write-combining into a faster store path. The text row also
includes 8 000 COM1 byte stores, which dominate the short framebuffer work.
The cache policy is therefore gated separately: `make x86-vga-smoke` reads
back `PAT=0x0007010600070406` (entry 5 = WC), the framebuffer PTE
`0x800000000000108b` (PAT, PWT, RW, P, PS and NX), an unchanged
`MTRR_DEF_TYPE=0x0000000000000c06`, a clean W^X audit and the same picture. A
second boot with `qemu64,-pat` requires the write-through PTE
`0x800000000000008b` and the same pixels.
The owner's laptop is the machine that can measure the real WC gain.

## Two RAM surfaces and the first present (2.1)

The measurement was taken on **27 November 2026** on an AMD Ryzen 5 5600 host
(AuthenticAMD, 12 logical CPUs), QEMU 11.1.3 TCG, one emulated processor,
256 MB and `-vga std`. The before tree is `e57cc873`; the after tree is
`0ad3e424`. Each side booted the shell-started desktop seven times, waited for
`wm: the desktop is up`, closed its two windows and read the TSC line from
`frames_report()`. No build or second QEMU ran beside a sample. The one-minute
load average during the series was 1.2-1.5 rather than the preferred below 1,
so these are lane numbers, not a release-gate replacement; the full
`make x86-vga-smoke` gate passed all 284 checks separately.

| Work | Before | After | Change | Spread before / after |
|---|---:|---:|---:|---:|
| `wm` first frame | **6.768024 ms** | **5.928155 ms** | -12.41 % | 2.59 % / 9.70 % |
| `wm` first present | not instrumented | **1.305167 ms** | — | 29.77 % |

The first-frame runs were 6 918 769, 6 815 402, 6 763 261, 6 743 212,
6 767 082, 6 794 724 and 6 768 024 ns before, and 5 824 633, 5 826 289,
5 928 155, 5 878 132, 6 001 009, 6 399 792 and 5 972 629 ns after. The first
present after the change was 1 276 604, 1 305 167, 1 300 157, 1 299 612,
1 325 247, 1 665 154 and 1 320 421 ns. Its wide spread is the shared host; the
first-frame medians are the comparison, not the fastest sample.

QEMU has neither Intel nor AMD page-flip callback in this run, so the after
side took the quiet software fallback. The flip-versus-copy difference itself
is expected to be small here: the video memory is ordinary RAM and TCG does
not model a separate VRAM traffic path. The measured first-frame improvement
is **not** a flip gain. It comes from the fallback's first present: one direct
whole-surface kernel copy replaces the old sequence of strip writes and the
userland transfer-buffer copies. After that first answer `uiprog` remembers
the refusal and later frames use the old damage-copy path.

On the laptop the expected gain is the removal of the steady-state 2 MiB RAM
copy, and only on the compatible Intel linear-RGB565 path. The first successful
flip still seeds the spare surface once, and the driver still waits for
`SURFLIVE`; that vsync wait is part of the measured present. A third surface
(or a composition task which runs during the wait) remains future work. The
reconnaissance's X-tiled XRGB8888 plane is incompatible and deliberately takes
the same copy fallback rather than changing mode or pixel format.

## Comparing two releases

The JSON is the machine-readable half of the same runs:

```bash
make bench BENCH_ARGS="--json /tmp/old.json"
git switch feature/bench            # or a worktree of the other release
make bench BENCH_ARGS="--json /tmp/new.json"
python3 - <<'EOF'
import json

def rows(path):
    with open(path, encoding="utf-8") as handle:
        return {row["name"]: row for row in json.load(handle)["results"]}

old, new = rows("/tmp/old.json"), rows("/tmp/new.json")

for name in old:
    before, after = old[name]["median"], new[name]["median"]
    change = (after - before) / before * 100.0 if before else 0.0
    print(f"{name:<16} {before:>12.2f} -> {after:>12.2f} {old[name]['unit']:<9}"
          f" {change:+6.1f}%  spread {old[name]['spread']:.1f} {new[name]['spread']:.1f} %")
EOF
```

`median` is the number the table prints and `value` is the same number (the
name of one run of the tool is older than the median), `values` holds the runs
behind it and `spread` is how far they were apart. A lane that claims a rate
says which of the two clocks the number is, and the JSON answers with
`runs` and `runs_asked`.

One run is one sample and the noise of the emulator is the spread in the table
above: `pipe`, `shell_pipe` and `lat_pipe_p50` are steady, `console` and
`file_read` are not, and `lat_syscall_p99` is the host's own scheduling. For a
release gate run each side three times and compare the medians; a row that
moved by a tenth is the noise of the day, a row that moved by half is a lane.

The host is shared with everything else on it. A build or a second QEMU beside
the run starves the emulator, and the guest clock and the host clock then part
company: the pipe of a run under load that took 21 s in the machine took
127 s on the host, and `syscall_getpid` fell by half. The tool compares the
two clocks of every run, prints `the host was busy in run 2: that run is not
in the median` under the table and leaves that run out (the JSON has
`"starved": [...]` and `"starved_runs": [...]`); a run that was starved is
thrown away and not compared, and a table whose every run was starved is a
failure of the tool.

## What the table does not cover

* **The rest of the latency.** Two operations are measured one at a time — a
  call of the door and a byte through a pipe — and the percentiles of a file
  write, of the console and of `fork_exec` are not in the table. `poll` is in
  the tree now and there is still no clock of a program finer than the
  millisecond, so every other row is a rate by design: the tail of a
  filesystem is a lane of its own.
* **The memory, the network and the many cores.** `mmap`, `brk` and the sockets
  are not in the tree of this port (the lanes M1 and M2 of `plan-1.6.md`);
  `KERNEL_SMP` is off by default and the run is one processor. A two-core table
  needs a second machine and a lane of its own, and the latency rows would be
  the first to move: a woken task on the core that is idle does not wait for
  the tick of the core that woke it.
* **The programs of somebody else.** The table is the kernel and the library
  of a program; the rate of zstd, SQLite or Lua is a number of those programs
  and is measured by their benchmarks, not here.
* **The disk behind the volume.** The root is MicroFS on a file of the host
  and the IDE channel of QEMU; the volume of the real machine (ADR 0029,
  `make x86-volume-smoke`) and its disk are another measurement, and the
  cache of the host is under both.
* **The board.** The ESP32 has no QEMU and no such table; its numbers are the
  ones of a session on the board.


## Intel display enable and present verification (2.1, Intel lane)

The measurement was planned on **27 November 2026** on QEMU 11.1.3 TCG,
`-vga std`, one emulated processor and 256 MB, the same machine as the
framebuffer and first-present rows above. `gpu display enable --yes` makes the
plane linear when needed and verifies that `k_blit_present` returns the device
path (rc=0) rather than the kernel software copy (rc=1). On this QEMU machine
there is no measured Intel ThinkPad 17aa:225c and the Intel page-flip backend
is not registered, so the device-present path is not available and the wm keeps
the software copy fallback. The QEMU delta for this lane is therefore expected
to be within noise; no QEMU device-present speedup is claimed here.

The number that matters is the laptop: with `gpu display enable --yes` active,
the 2 MiB-per-frame software copy should disappear from the wm present path.
The owner checklist is to boot the laptop, run `gpu display enable` (dry-run),
then `gpu display enable --yes`, confirm the log line `gpu: display enable:
present verified (device path, rc=0)`, and compare the wm first-present TSC line
before and after enable. If the screen goes black, reboot restores the firmware
plane.

| Work | QEMU (no Intel flip backend) | Laptop (owner) |
|---|---|---|
| `k_blit_present` path | kernel copy (rc=1) | device (rc=0), after enable |
| 2 MiB copy per present | present | removed by page flip |
| `wm` first present | unchanged in character | measure TSC ns before/after |

The next step, not done in this lane: GTT/VM for own RAM buffers and Y/Yf
tiling, so the detile copy is removed as well.
