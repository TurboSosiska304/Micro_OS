# The plan of 1.6

> The release where the machine is in the network and a program lives in the
> memory it asks for. It follows 1.5.0 (`docs/releases/1.5.md`), which made a
> program a process, a task a user, the filesystem complete and the disk
> faster than the 1990s.

The lanes are independent unless the text says otherwise, each one is a branch
off `release_candidate`, and each one carries its own gates. What does not fit
in 1.6 moves to 1.7 without rewriting the promise.

## M1 - `poll`, and a memory a program can map

The first lane, because the others stand on it.

* **`poll`** takes the next free syscall number, **43**: an array of
  `struct pollfd`, a timeout in milliseconds, `POLLIN`/`POLLOUT`/`POLLERR`, and
  a zero count that is a sleep. The wait is of the scheduler and not a loop of
  the task, so a program of the console waits with the others.
* **`mmap`/`munmap`**: a file and anonymous memory, `MAP_SHARED`/`MAP_PRIVATE`,
  `PROT_READ`/`PROT_WRITE`, pages taken on the fault from the filesystem, and
  `msync` for the shared ones.
* **`execve` without a second copy**: the image is mapped from the file, not
  read into a buffer and copied; a write of a page is a copy of that page.
* the heap: `brk` grows without crossing the stack, and the refusal is `ENOMEM`
  with a sentence and not a panic.

Gates: a program reads a file of 64 MiB through `mmap` and writes a shared
mapping that `e2fsck` and `cat` see afterwards; `poll` on two pipes, a socket
and a timeout; the memory of a run of a program of 4 MiB measured before and
after; `make test`, `test-asan`, `test-smp`, the smokes of 1.5 and no fewer
checks than they had.

## M2 - an HTTP server and an update over the network

After M1, because `httpd` serves many connections.

* **`/bin/httpd`** is a program and not the kernel: GET and HEAD, keep-alive,
  a file of the volume, a directory listing, `ETag` and `304`, `Range`, a
  ceiling of connections, a timeout, and a line of the log per request.
* **TLS** for it on x86-64 (the client has had it since 1.2.0).
* **an update over the network** into the inactive slot of the board
  (`ota_0`/`ota_1`): the signed image, the write, the reboot, and the
  **rollback** when the boot does not confirm itself. Without the rollback the
  lane is not done.
* **MQTT is stopped** (the decision of 23 November 2026): no broker, no topics,
  no QoS in the x86 port. The client of `components/net/` stays on the shelf
  for a decision of its own.

Gates: a file of 100 MiB served and compared byte for byte, 100 connections at
once, `curl -k` over TLS, an update of the board from a local HTTPS server and
a rollback of a broken image.

## M3 - bus-master DMA

The port has no contract of a physical address to hand a controller; that is
an ADR before it is a driver.

* an ADR for the physical address of a buffer of the kernel and of a page of a
  program (the page tables of the port are the only place that knows);
* the PCI IDE controller, a PRDT of 4 KiB entries, IRQ 14, a wait on the
  interrupt instead of the poll, and a fallback to PIO for a controller that
  does not do it;
* a write-back cache with a count of the dirty blocks in `/proc`.

Target: **at least 80 MiB/s** of writing on the volume of 5 GiB, against the
8.4 MiB/s of 1.5.0.

## M4 - the debts of the many cores

* the **I/O APIC**: the timer and the keyboard off the 8259, an entry per line,
  and the delivery to more than the first core;
* the **wake IPI**: a task enqueued on another core wakes that core instead of
  waiting for its next tick;
* a **guard page** under the stack of a task: an overflow is a fault and not a
  write into the heap.

## M5 - the second half of the users

* `/etc/passwd` (a name, a uid, a gid, a home and a shell) and `/etc/shadow`
  with a hash;
* `login` and `su`, and a console that starts as root and hands the line to a
  user;
* the **setuid bit** of an image (`MOX1`) and a **gid in a record** of the
  filesystem: both are format changes and both need an ADR before the code.

## The gate of the release

`make test`, `test-asan`, `test-smp`, `test-minimal`; `x86-smoke` no fewer
than 323; `x86-vga-smoke` 129; `x86-fs-smoke` 14; `x86-disk-smoke` 57;
`x86-net-smoke` 57 plus the scenarios of M2; `x86-root-smoke` 47;
`x86-smp-smoke` 37/37; `smoke` 69; `x86-volume-smoke` 19; `idf-build`; the two
sessions of 600 seconds; the board; a Release build from an empty directory;
`integrity --expect-tag`.
