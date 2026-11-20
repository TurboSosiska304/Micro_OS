# The host port: the tests and the simulator

A port of the kernel for a Linux process: threads instead of a scheduler
substrate, `pthread` mutexes instead of a spinlock, files and sockets instead
of a flash and a radio. It is the development loop - the kernel and the
portable components build and run on a laptop, and the checks run there.

| | |
|---|---|
| port | `ports/port_host` (pthreads, files, sockets, a PSRAM pool in the heap) |
| builds | `make test`, `make test-asan`, `make test-tsan` |
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

The tests take a volume and a store of their own, so the runs do not depend on
each other.

## What it cannot do

* run a program: `k_port_exec_supported()` is false - a test harness must not
  jump into an arbitrary image;
* reproduce preemption: the critical section of the port is a recursive
  `pthread` mutex, so a race of the kernel is invisible unless the race
  detector looks for it (`make test-tsan`).
