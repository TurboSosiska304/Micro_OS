# The host port: the tests and the simulator

A port of the kernel for a Linux process: threads instead of a scheduler
substrate, `pthread` mutexes instead of a spinlock, files and sockets instead
of a flash and a radio. It is the development loop - the kernel and the
portable components build and run on a laptop, and the checks run there.

| | |
|---|---|
| port | `ports/port_host` (pthreads, files, sockets, a PSRAM pool in the heap) |
| builds | `make test`, `make test-asan`, `make test-tsan`, `make test-smp` |
| simulator | `make sim`, `make smoke` |

## The simulator

`make sim` boots the **whole portable system**, not only the shell: MicroFS on
a file volume, `/proc`, `/dev`, `/etc` restored from the store, `init`, then
the shell. `make smoke` drives it with a list of commands and checks the
transcript; `tools/sim_smoke.py` is that test.

The environment shapes it:

| Variable | Meaning |
|---|---|
| `MICROOS_VOLUME` | the file of the volume (default `/tmp/microos-sim.img`) |
| `MICROOS_KV_DIR` | the directory of the key-value store (default `/tmp/microos-kv`) |
| `MICROOS_SCREEN` | draw the console into this file instead of nowhere |
| `MICROOS_INPUT` | a script of button events for `input` |
| `MICRO_OS_CPUS` | the cores of the kernel, 1 by default (2 in `make test-smp`) |

The tests take a volume and a store of their own, so the runs do not depend on
each other.

## The cores

Every task of this port is already a pthread, so the machine is as parallel as
the operating system makes it. `MICRO_OS_CPUS` tells the kernel how many cores
it has; the number becomes the array of `struct k_cpu` and the run queues of
`KERNEL_SMP` (ADR 0012). It is read once, when the kernel initialises.

A "core" here is a run queue and an identity a task carries, not a worker
thread: the port does not pin a task to a processor and does not schedule it
(the operating system does). A task is never on two cores at once - it is one
pthread with one `cpu` field, and only the kernel scheduler writes it - but two
tasks of the same core can really run at the same time, which a kernel of two
real cores would not allow. That is the price of one pthread per task, and the
thread sanitizer is the answer to it: `make test-tsan` and `make soak-host`
run the tests and the stress on two cores, and the stress migrates tasks
between them while it works.

`make test-smp` is the plain build with `KERNEL_SMP=ON`; the test property of
`tests/CMakeLists.txt` sets `MICRO_OS_CPUS=2` for it. The kernel of one core
(`make test`) is the kernel of 1.0 and its tests are unchanged.

## What it cannot do

* run a program: `k_port_exec_supported()` is false - a test harness must not
  jump into an arbitrary image;
* reproduce preemption: the critical section of the port is a recursive
  `pthread` mutex, so a race of the kernel is invisible unless the race
  detector looks for it (`make test-tsan`).
