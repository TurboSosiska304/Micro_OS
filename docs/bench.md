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

Ten rows are the clock of the machine (the program measures itself) and one,
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

The pipe row reads the scheduler and not the copy, and it is worth a second
look: **about 1 ms, one tick of the timer.** A blocked reader is woken by the
write in the kernel, but a wake only moves the tick of the sleeper
(`components/kernel/src/wait.c`: a waiter asleep is woken by the scheduler at
its next tick), so the read returns about one tick later and the state of the
task, not the copy of the byte, is the number. The rate row, `pipe`, does not
see this: its two ends retry `EAGAIN` and never sleep, and 4 MiB of 256-byte
handoffs at 31 MiB/s is 8 µs for a handoff. The two numbers do not disagree;
they measure a busy pipe against an empty one, and the wake that has to wait
for the tick is what a program that blocks on a pipe pays.

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

The pipe's p99 is the other way around: the tick is a hard floor under the
handoff and the tail is 3 % of the middle, so the two rows move together and
the p50 is the row of the gate.

## The gate of a release

The gate of a release is **three runs and the median of each row**: `make
bench` with no arguments. A row that moves by less than a tenth between a
release and the one before it is not a signal; a row that moves by more is a
claim that has to survive the spread of the runs.

Six rows are the gate of a release, and a release does not pass while one of
them is missing or zero. (The tool refuses a table with any row missing; these
are the ones the gate is read by.)

| Required row | What it answers |
|---|---|
| `syscall_getpid` | the door of the system call and its cost |
| `fork_exec` | a task, an image and the wait of a parent |
| `pipe` | the handoff of the pipe and the scheduler behind it |
| `file_write` | the allocator and the writer of the filesystem |
| `file_read` | the reader and the cache of the host |
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

* **The pipe holds 256 bytes** (`KFILE_PIPE_SIZE`, ADR 0014) and answers
  `EAGAIN` after a bounded wait. The 4 MiB of `pipe` is therefore sixteen
  thousand handoffs of 256 bytes, and its rate is the scheduler's, not the
  copy's: 0.2 MiB/s is 256 bytes per ~1.3 ms. `shell_pipe` pays the same
  shape through the library of `cat` and `wc`. The loop of the benchmark does
  not change when the pipe grows a real buffer - it retries on `EAGAIN` and
  on the short read and the short write - so the row moves and the test does
  not.
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
