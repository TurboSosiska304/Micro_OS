# The numbers of the machine

A release is compared with the one before it by a table of numbers and not by
an impression, and the table has to be the same table every time. `make bench`
is that table: one boot of the x86-64 port, a fixed set of work, and one rate
for each piece of the kernel the work leans on.

The program that measures is `ports/port_x86_64/programs/bench.c`; the tool
that writes the image, boots QEMU once and reads the lines back is
`tools/x86_bench.py`. The program is **not** in the image of the release: the
tool puts it there for the run, the way `tools/x86_volume_smoke.py` puts
`/bin/mkbig`, because a measurement is a tool of the gate and not a command of
the machine.

## The run

```bash
make bench                                  # the table on the console
make bench BENCH_ARGS="--json bench.json"   # the same table as JSON as well
make bench BENCH_ARGS="--fs ext2"           # the same tree on an ext2 root
make bench BENCH_ARGS="--verbose"           # the console of every line
```

One run is about a minute of emulation and is **not** a part of `make ci`: it
is a release gate, like `make x86-volume-smoke`, and not a check of a push.
`make bench` builds what it needs (`x86-build`, `build`) and writes a volume of
64 MiB in a directory under `build-x86/` that it removes when the run ends
(`--keep` or `--dir` keep it).

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
| `shell_pipe` | 4 MiB | `cat /bench.bin \| wc -c` |

## The table

Every row is one line of the console, and the shape of the line is the
contract between the program and the tool:

```text
bench: <name> <value> <unit>
```

The value has two decimals and the unit is one of `calls/s`, `runs/s`,
`MiB/s` and `lines/s`. The program prints a `bench note: <name> <count>
<what> in <ms> ms` line beside it with the bytes and the milliseconds behind
the rate; the tool ignores it and prints it in the human summary. A failure
of a benchmark is a line `bench error: ...` and a status of the program that
is not zero, which is the answer of the tool as well.

Six rows are the clock of the machine (the program measures itself) and one,
`shell_pipe`, is the wall clock of the host, because the shell pipeline prints
no rate of its own. The tool prints the seconds of the host beside every row
of the human summary so that the two clocks are never confused.

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

The JSON is the machine-readable half of the same run:

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
    before, after = old[name]["value"], new[name]["value"]
    change = (after - before) / before * 100.0 if before else 0.0
    print(f"{name:<16} {before:>12.2f} -> {after:>12.2f} {old[name]['unit']:<9}"
          f" {change:+6.1f}%")
EOF
```

One run is one sample and the noise of the emulator is the spread in the
table above: `pipe` and `shell_pipe` are steady, `file_read` is not. For a
release gate run each side three times and compare the middle of the three;
a lane that claims a rate says which of the two clocks the number is, and how
many runs it took.

## What the table does not cover

* **Latency.** Every row is a rate of many operations; the worst call, the
  longest wait and the 99th percentile of a file write are not in the table.
  There is no `poll` and no clock with a resolution finer than a millisecond,
  so a call of no work is timed in a loop and not one at a time.
* **The memory, the network and the many cores.** `mmap`, `brk`, `poll` and
  the sockets are not in the tree of this port (the lanes M1 and M2 of
  `plan-1.6.md`); `KERNEL_SMP` is off by default and the run is one
  processor. A two-core table needs a second machine and a lane of its own.
* **The programs of somebody else.** The table is the kernel and the library
  of a program; the rate of zstd, SQLite or Lua is a number of those programs
  and is measured by their benchmarks, not here.
* **The disk behind the volume.** The root is MicroFS on a file of the host
  and the IDE channel of QEMU; the volume of the real machine (ADR 0029,
  `make x86-volume-smoke`) and its disk are another measurement, and the
  cache of the host is under both.
* **The board.** The ESP32 has no QEMU and no such table; its numbers are the
  ones of a session on the board.
