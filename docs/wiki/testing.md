# Testing

The checks are layered by what they can see. A unit test sees one piece of the
kernel; the simulator sees the whole portable system as a process; the x86-64
smoke test sees a bare machine and a processor; the device test sees the board
and the chips on it. None of them replaces another.

## The commands

| Command | What it proves | Needs |
|---|---|---|
| `make test` | the host build, the unit tests, the null probe | `gcc`, `cmake`, `ninja` |
| `make test-asan` | the same tests with the sanitizers | the same |
| `make test-tsan` | the same tests under the race detector | the same |
| `make test-minimal` | the host tests with the optional features off | the same |
| `make smoke` | the simulator boots and the shell answers | the host build |
| `make coverage` | the lines of `components/kernel` and the threshold | `gcc`, `gcov`, Python |
| `make reproducible` | two runs of the disk image are the same bytes | `gcc`, `cmake`, `ninja` |
| `make x86-build` | the x86-64 kernel and loader compile | `gcc`, `cmake`, `ninja` |
| `make x86-smoke` | the kernel boots, runs `/bin/hello`, the shell answers | `qemu-system-x86_64` |
| `make x86-vga-smoke` | the VGA screen and the PS/2 keyboard, no display | `qemu-system-x86_64` |
| `make x86-disk-smoke` | a file written by one boot is read by the next | `qemu-system-x86_64` |
| `make x86-net-smoke` | the network: ping, UDP, HTTP, `httpd` and DNS | `qemu-system-x86_64` |
| `make integrity` | the boundary, the bundle, the partitions, the hygiene | `clang-format` |
| `make ci` | the list a contributor runs before a push | the above, QEMU optional |
| `tools/device-test.sh` | build, flash and boot the board | ESP-IDF and a board |

## `make test`: the unit tests

`make test` configures and builds `build-host/` and runs `ctest`, which runs
three tests:

* `microos_tests` - the unit tests, in `tests/cases/`, on the framework of
  `tests/framework/ktest.h`. There is no external dependency; the tests run
  inside a real kernel task, so they exercise the task API as well.
* `mkprog` - `tools/test_mkprog.py`, the layout of the image that
  `tools/mkprog.sh` writes; it builds tiny objects with the compiler of the
  host and skips itself when Python is not found.
* `null_probe` - the kernel and every portable component linked against the
  null port. A symbol that portable code takes from a machine by accident is a
  link error here, on a laptop.

The numeric corpus is `tests/corpus/vectors.txt`: 512 numbers with the
boundary values (0, 1, 255, 65535, 2147483647 and so on) mixed in. A new
numeric behaviour reuses it rather than inventing a second list.

`make test-asan` and `make test-tsan` are the same tests in `build-asan/` and
`build-tsan/`. The board has no debugger, so the host is the place where a
wrong pointer or undefined behaviour has to be caught; TSan is the only thing
that sees a race, because the host port cannot reproduce preemption.

`make test-minimal` is the second configuration of the host build (ADR 0010):
`KERNEL_PSRAM`, `KERNEL_EXEC`, the network components and the second
filesystem are off, in `build-minimal/`. It proves that the sources still
build without them and runs the tests that do not need the feature - the
cases of `exec` and of the network are guarded in the source, so the report
still says how many checks the small build has.

## `make smoke`: the simulator

`make smoke` runs `tools/sim_smoke.py`, which boots
`build-host/ports/port_host/microos_sim` with a volume and a store of its own,
types a list of commands and checks the transcript: the subsystems, MicroFS,
`/proc`, `/dev`, the configuration of `/etc`, the shell and the network. It is
the cheapest end-to-end test of the portable system, and it takes a couple of
seconds.

The simulator links `components/net`, so the same HTTP client, clock of NTP
and page of the state the board runs over lwIP run there over the sockets of
the process. The smoke test starts a small page of its own on the loopback and
asks the guest for it with `http get`, so the path from the shell through the
client to a real socket is exercised, not only the parser of a unit test. It
does not open a port on the network of the machine: the page lives in the test
process, on a port the kernel chose.

`make sim` runs the same binary by hand, with `/tmp/microos-sim.img` and
`/tmp/microos-kv` by default. `python3 tools/sim_smoke.py --verbose` shows
every step.

## `make reproducible`: the same inputs, the same bytes

The gate of 1.0 asks for an image that two runs build to the same bytes. Three
things used to make that fail:

* the records of MicroFS are stamped with `mtime` and `ctime`, and the
  filesystem took the moment from the clock of the process;
* `kernel_build_id()` is `__DATE__ " " __TIME__`, which changes on every
  compile;
* an absolute path of the source tree reached the debug information and
  `__FILE__` of a panic message.

`make` exports `SOURCE_DATE_EPOCH` (0 by default, the convention of
reproducible builds); `make reproducible` builds the disk image, keeps a copy,
builds it again from nothing and compares the two with `cmp`, then prints
their `sha256`. What is fixed:

* `tools/mkfs_mfs.c` reads `SOURCE_DATE_EPOCH` (and `--source-date-epoch`) and
  gives the filesystem one fixed moment for every record
  (`mfs_set_time_source()`); the running machine keeps the wall clock of the
  kernel;
* `cmake/reproducible.cmake` sets `__DATE__` and `__TIME__` from the same
  moment and maps the paths of the tree with `-ffile-prefix-map`, so the
  kernel id and the debug information no longer depend on when and where the
  build ran;
* `tools/bundle.sh` makes its archive with sorted names, a fixed mtime and the
  numeric owner 0, and `gzip -n`; the ESP32 firmware uses
  `CONFIG_APP_REPRODUCIBLE_BUILD` in `sdkconfig.defaults`.

A `sdkconfig` that was generated before this option existed keeps its old
value: the defaults of the tree are read only when there is no `sdkconfig`.
On an old checkout, remove `sdkconfig` (or set the option in `idf.py
menuconfig`) before the build that has to be reproducible.

`SOURCE_DATE_EPOCH` changes the bytes of the image on purpose: two dates are
two different builds. What is compared is two runs with the same date, the
same sources and the same tools.

The board is the other half. The cloud builds the firmware twice, in `build/`
and `build-repro/`, and compares `micro_os.bin`, `bootloader.bin` and
`partition-table.bin`; it makes the bundle twice as well and compares the two
`dist/` trees, archive included. Two clean builds of this tree with
`SOURCE_DATE_EPOCH=0` gave the same three images. A laptop without ESP-IDF can
only do the half above, which is why `make reproducible` is the image and not
the firmware.

## `make coverage`: the lines of the kernel

`make coverage` configures `build-coverage/` with `-DMICRO_OS_COVERAGE=ON`
(`--coverage -O0 -g`), runs the unit tests and the simulator, and then
`tools/coverage_report.py` hands every `.gcda` to `gcov --json-format` and
merges the reports by `(source, line)`, so a source compiled into two binaries
is counted once. A line is covered when any of the runs reached it. The
report counts `components/kernel` and fails under the threshold: **70% of
lines**. This tree met **80.6-80.7%** (2581-2584 of 3201 lines) when the gate was added;
70 leaves room for another compiler and another `gcov`, whose notion of an
executable line can differ a little, and is high enough that a regression of
the tests is noticed. `--path` and `--threshold` move the report to another
subtree or another number.

`gcov` is a part of GCC and no `lcov` is needed: a machine with the compiler
has the report. The counters are removed before the run, so the number is of
this run and not of the runs before it.

## The x86-64 smoke tests

`make x86-smoke` boots the kernel on a real (emulated) processor: QEMU loads
the loader of the port, the loader turns long mode on and copies the kernel,
and the kernel prints its banner on the serial port. Then the script types
commands into COM1 and checks the answers: the boot, the filesystem, the
program of `/bin` (the `execve` path, the arguments, the status), the fork with
copy on write, the fault of `/bin/crash`, and the `EFAULT` of `/bin/badptr`.
The run of this tree passes 48 checks and ends with `RESULT: PASS`.

`make x86-vga-smoke` looks at the other console: it boots with `-display none`,
reads the text memory at `0xB8000` through the monitor of QEMU, takes a
screenshot and types with `sendkey`, so it proves the screen and IRQ1 without a
display server. It is a target of its own and **not** a part of `make ci`: the
monitor of QEMU is a tool a build farm does not have to have.

`make x86-disk-smoke` gives the machine an 8 MB image on the primary IDE
channel, writes a file in one boot and reads it in the next. It is the test of
the ATA driver, of the MBR parser and of MicroFS on a real medium, and it is
also not a part of `make ci`.

`make x86-net-smoke` builds the image with `-DMICRO_OS_X86_NET=ON`, gives QEMU
a virtio-net card on its user-mode network and a forward of a port of the host
to the guest, and checks the whole contract of the port over the wire: the
device and its MAC, `ping` of the gateway, a UDP exchange with an echo server
of the host, an HTTP GET of the client of `components/net`, the page of the
state served by the guest through `hostfwd`, and a name the resolver answers
twice (the second from the cache). Like the other two it is not a part of
`make ci`: it opens a port on the loopback and needs the user-mode network.

## `make integrity`: the boundary and the hygiene

`tools/integrity_check.py` checks what a compiler cannot:

* the **layers**: the kernel includes only the kernel, a portable component no
  platform header and no `k_port_*` call, and a port implements the whole
  contract of `kernel/port/*.h`;
* the **bundle** in `dist/`, when there is one: the sums and the images;
* the **partition table**: `partitions.csv` against the flash size and the
  build;
* the **budget** of the image: the firmware fits its 2 MB slot;
* the **features** (ADR 0010): a flag that is off leaves no symbol in the map
  of the ESP-IDF build and no library in the host build;
* **git**: the objects, through `git fsck --strict`;
* the **hygiene** of the sources: UTF-8, LF, no trailing whitespace, no tabs,
  100 columns in C, and `clang-format` when it is installed.

`--skip-format --skip-git` and `--expect-version <version>` are the useful
options when a release is prepared; `--verbose` names every check.

## `make ci`: the local half

```text
build, test, test-asan, test-tsan, smoke, coverage, [x86-smoke], reproducible,
integrity
```

`x86-smoke` joins the list when `qemu-system-x86_64` is on `PATH` and is
skipped with a message when it is not: a missing optional machine must not fail
a run. `coverage` and `reproducible` are blocking: the first refuses a kernel
below 70% of lines, the second refuses two images that differ. `make ci` does
**not** build the firmware of the board (that needs ESP-IDF) and does not run
the VGA or disk smoke tests or the device test.

The cloud repeats the list on every push to `mainline` and `release_candidate`
and adds what a build farm has: valgrind over the tests, the firmware build,
the bundle, and `x86-smoke`. The local half and the cloud half are close but
not identical; [`docs/debts.md`](../debts.md) keeps the difference as a debt.

## The device test

```bash
tools/device-test.sh                  # auto-detected port
tools/device-test.sh /dev/ttyACM0
CONSOLE_SECONDS=60 tools/device-test.sh /dev/ttyACM0
```

The script finds ESP-IDF (`$IDF_PATH`, `../esp-idf`, `~/esp/esp-idf`), builds,
flashes, reads the console for `CONSOLE_SECONDS` (30 by default) and prints
`RESULT: PASS` when the self test, the hardware doctor and the shell came up.
It is outside CI: no runner has the board. A release is gated on it by hand.

`tools/console.py` reads the console without flashing; `tools/stream_console.py`
is the interactive version. Both need `pyserial`, which the ESP-IDF Python
environment already has.

## Adding a test

A new case needs four touches (the same list as in
[`AGENTS.md`](../../AGENTS.md)):

1. `tests/cases/test_<thing>.c`;
2. the `void test_<thing>(void);` declaration in `tests/include/tests/tests.h`;
3. the call in `run_tests()` in `tests/src/main.c`;
4. the file in `add_executable(microos_tests ...)` in `tests/CMakeLists.txt`.

Kernel behaviour without a test is unfinished work. A test that needs real
hardware is a sign that platform and portable code got mixed: the kernel tests
run on the host, and the board is checked by the device test and by the smoke
tests of its port.

The fuzz of the ABI and of `MOX1` has a deterministic seed
(`tests/cases/test_fuzz.c`); the records of MicroFS have none yet, which is an
open row of the register.

[Back to the wiki index](README.md)
