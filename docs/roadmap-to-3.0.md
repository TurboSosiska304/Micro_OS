# The gate of 3.0 (a proposal)

* Status: **a proposal**, not a promise; the version that takes it writes its
  own plan and names this file.
* Related: `docs/roadmap-to-2.0.md` (the line to 2.0), `docs/plan-2.0.md` (the
  lanes of 2.0), `docs/adr/0025-copyleft-outside-the-tree.md` (licences),
  `docs/adr/0011-versioning-and-1.0.md` (what a version number promises),
  `docs/debts.md` (what is open).

## The framing

2.0 is **the machine of its user**: one program cannot reach another, a copy of
a process is a copy of its pages, two accounts carry a group and a `setuid` bit,
a desktop starts with the machine, and the wire has an address nobody typed.

3.0 is **the machine of a stranger**: it can be installed, upgraded, trusted and
extended by a person who has never seen this tree. That is not a list of
features - a major release of a system is a promise about what will not break -
so the gate of 3.0 is a list of **guarantees**, each one with a gate that a
machine runs and a number that a person reads.

A gate of 3.0 is green only when all ten are green, together, on the same tree,
on a quiet host, in the order below. Each gate names the command, the number
that makes it pass, and the defect it would catch.

## 1. An upgrade across a major, with the data

A major may break a format; it may not lose a file. The image of 2.x with a
volume of files is booted, upgraded in place to 3.0, and booted again: the
files are where they were, the accounts are the accounts, and the volume
format is the new one. An upgrade that fails at any step leaves the machine on
2.x with the volume intact.

* **Gate**: `make upgrade-smoke` - an image of 2.0 with `/home`, `/etc`, a
  volume of MicroFS version 4 and one of ext2; the upgrade; the boot of 3.0;
  every file has the digest it had; then the same with a failure injected at
  each step (a full volume, an unreadable block, a killed update task) and the
  machine boots 2.0 with the old digests.
* **Catches**: a format change that was not migrated, an update that leaves a
  half-written volume, a rollback that does not.

## 2. Compatibility measured, not promised

The promise of ADR 0011 ("the syscall numbers and the image format change by
adding only") is a sentence until a binary of 1.x runs on 3.0.

* **Gate**: `make abi-smoke` - five program images built by releases 1.4, 1.7,
  1.8, 1.9 and 2.0 (they are the artifacts of those tags) are installed on 3.0
  and run: each prints what it printed then, and `exec` refuses an image of a
  version it cannot run with a sentence that names it.
* **Catches**: a syscall number reused, a header of MOX1 read differently, a
  program that silently does the wrong thing.

## 3. A program of somebody else, doing a day of work

`ports-smoke` of 1.8 proved thirteen utilities with no patch. A major is the
release to prove a **suite**: a project of the outside world, built unchanged,
doing a workload and not a hello.

* **Gate**: `make ports-suite` - permissive programs with a fixed workload:
  the whole console set of `sbase`, `9base`'s `sam`/`ssam` or `pkgconf` and
  `bearssl` as a library, `lua` and `sqlite` on a database of a thousand
  records, `zstd` on a file of 64 MiB, and `kilo` editing a file. The table of
  `docs/ports-table.md` reaches **zero patches** for the suite, and
  `tests/ports/upstream.txt` holds the digest of every file.
* **Catches**: the corner of POSIX that only a real program uses.

## 4. A campaign of robustness

A journal makes a power cut survivable (1.9). A major says the system does not
panic under anything else either.

* **Gate**: `make fuzz-24h` - a night of the fuzzers of the tree (a MicroFS
  image, an ext2 image, a MOX1 image, a packet of the network, a line of the
  shell) with **zero panics** and every rejection a sentence; and
  `make fault-smoke` - twenty-two injections (a block device that fails a read,
  a write, a flush; the allocator at zero; a task killed mid-write; a signal
  storm; a timer that does not tick; a disk unplugged under a mount), each
  ending with the program dead and the shell alive.
* **Catches**: the untested `if` in an error path, a null pointer after a
  failed allocation, a mutex held across a return.

## 5. Reproducible, signed, and honest about licences

* **Gate**: `make reproducible` on the three ports (the same sources, the same
  `SOURCE_DATE_EPOCH`, the same bytes twice), `make verify-release` (the
  signature of every artifact checked against `microos-sign.pub`, and the
  checksums of the bundle), and `make licenses` (every binary's licences
  listed from `docs/licensing.md`: **zero** copyleft in a binary of the tree,
  ADR 0025).
* **Catches**: a build that depends on the machine, a release whose bytes
  nobody can reproduce, a GPL file that arrived in `third_party/`.

## 6. A stranger installs it in ten minutes

* **Gate**: a cleanroom agent, given a copy of `docs/install.md` and a machine
  with the image, installs the system, boots it, logs in, writes a file,
  reboots and finds it - **with no chat and no hint**. The report names every
  place the guide was wrong; the gate is a report with **zero blockers**.
* **Catches**: a guide written by the person who knows the tree, for the person
  who knows the tree.

## 7. A floor under the numbers

A major may be slower only by a decision that is written down.

* **Gate**: `make bench --floor` with the thresholds of 2.0: `lat_pipe_p50` at
  most 50 µs, the write of a volume of 5 GiB at least 100 MiB/s, a mount of a
  volume of 5 GiB at most a second, a fork of 64 MiB at most a millisecond. A
  number that falls more than 10 % below the median of 2.0 fails the gate
  unless the note of the release says why.
* **Catches**: the regression that nobody measured, the second copy of a buffer
  that cost 20 % and "looked fine".

## 8. A policy of security with its cases

* **Gate**: `make security-audit` - `SECURITY.md` names the model of the
  threat, the process of a disclosure, and the classes of defect that are
  **impossible** by construction, each with a case that tries: a page that is
  writable and executable, a write of ring 3 into the kernel, a task that
  reads the stack of another, a `setuid` program that escalates, a path that
  leaves its root through `..`, a file of a group that its neighbour writes.
  Every case must fail in the right way, with the sentence of the reason.
* **Catches**: the promise of 2.0 that was true in a test and not in a corner.

## 9. Two people on one machine

* **Gate**: `make users-smoke` - two accounts, two homes, a session each (the
  console and one of the wire); a suite of attacks (a path that climbs out, a
  `uid` that is asked for, a `setuid` bit that is copied, a group that is
  joined without a right), each refused with a reason; the files of one are not
  readable by the other, and the digests of the home of each are the same
  before and after the other's session.
* **Catches**: the permission that is checked at the door and not on the way.

## 10. The board, still

The board is a compatibility port (ADR 0018): it must still boot, still pass
its self test, and still update with a rollback - or it is not a port of this
system any more, and the release says so and drops it.

* **Gate**: `tools/device-test.sh` green, a **24-hour** `make board-soak` with
  zero errors, an update over the USB that rolls back when the new image is
  broken, and `idf-build` with the budget of the image checked.
* **Catches**: the drift of a port nobody runs.

## 10a. The machine keeps its own log (2.1)

Proposed by the owner on 25 November 2026, for the release after 2.0: the
capture of the kernel log that already exists in RAM (`g_capture`, the boot
until the console exists) and the stream of the console are written to the
root volume.

* `/etc/bootlog.log` - the captured boot of this start, written once the root
  is up and mounted read-write; the previous one becomes `bootlog.1.log`, so a
  machine that does not start still leaves the boot of the attempt before it.
* `run.log` - the console of the session, appended: a person who reports "it
  hung" can say where, and a test of the real machine has a transcript.
* **The rule of the password**: the input of a password is never written -
  the log keeps what the machine printed, not what a person typed at a prompt
  of `login`, `su` or `passwd`. A log that leaks a password on a shared machine
  is worse than no log.
* A read-only root (the ISO, `boot0p1` of the stick) keeps the log in RAM and
  says it does; the board has `/etc` in the NVS and no volume to append to, so
  it keeps the same log in RAM until a store exists.
* The size is bounded: a rotation at 64 KiB per file, two files, and a line
  that says a file was rotated.

## 10b. The language of the tree moves to C23 (2.1)

Measured on 26 November 2026, not estimated: the host build (GCC 16.2.1) and the
x86-64 port (clang 23.1.5) **compile with `-std=c23` and
`CMAKE_C_EXTENSIONS=OFF` and answer zero errors**, so the tree is already C23
clean. The change is `CMAKE_C_STANDARD 23` in the two CMakeLists and a gate;
the board stays on the dialect of ESP-IDF 5.3 (its Xtensa GCC knows C23 only as
`-std=c2x`) and that is written down as the reason.

It belongs to 2.1 and not to 2.0.2 on purpose: a release that fixes the desk
and the shutdown of a real laptop must not also change the dialect of every
translation unit. What C23 buys the tree: `#embed` (the font and the tables
without a generator), `[[noreturn]]`/`[[fallthrough]]`, `nullptr`,
`constexpr`, `typeof`, `stdbit.h`, and `0b1010`.

## 11. The packages of a person (promised to 3.0)

The person who uses 3.0 must be able to install **base applications from the
repository of the project** and a **defined list of programs of BSD and Linux**,
without building them by hand and without a toolchain on the target.

* **Gate**: `make pkg-suite` - a fresh machine of 3.0, `pkg repo add` of the
  repository of the release, `pkg -Sy`, then `pkg -S` of the list:
  * **the base applications of this project**: the editor, the file manager, the
    terminal, the shell tools of `/bin/ports`, the calculator, the archiver -
    the programs of the tree that a person of a desktop uses;
  * **the list of BSD and Linux programs**, versioned in `docs/packages.md`
    with the name, the version, the licence and the digest of the source: the
    permissive ones built by the tree's SDK with zero patches (sbase, 9base,
    `less`, `neatvi`), and the copyleft ones (`nano`, `vim`, `git` if it fits)
    **built outside the tree** by the repository - ADR 0025 allows a GPL
    program as a package of the system and never as a file of the tree.
  Each package is installed, run on a fixed input, and its answer checked; an
  upgrade of a package to a new version, and a rollback of a broken one.
* **Catches**: a package that needs a library the image does not have, a
  repository that signs the index and not the sources, an install that leaves
  the volume half-written.
* **The repository** (`microos-progs`, ADR 0024) carries the packages, the
  index and its signature; the list of `docs/packages.md` is a promise of the
  release and is checked by the gate on every `rc`.

## 12. A beginning of a GPU, expected and not promised

A release line that says "the desktop" and no word about a graphics processor
is a line that has not looked at the horizon. 3.0 **does not promise** a driver
of a real GPU - a driver of Intel or AMD is a firmware blob (not free, ADR
0025), a memory manager of its own and years of work - but the release line
**expects to begin it**, and the shape of the beginning is:

* a driver of **`virtio-gpu` 2D** behind `kernel/fb_ops.h` (the contract of
  1.7, which the software path is the only implementation of today): a command
  queue, resources, a framebuffer, a scanout - all of it in QEMU, all of it
  checkable, and it is the machine the compositor of the desktop will use to
  prove that a real scanout is not slower than a copy;
* an **ADR** that says what a real GPU would need (firmware, a memory manager,
  a command submission model, and the licence of the firmware), so that the
  next line begins with a document and not with a surprise;
* a gate, if the driver lands: `make gpu-smoke` - the desktop of 2.0 drawn by
  `virtio-gpu`, a full frame and a damaged one, and the copy path of the
  software fallback as the reference.

What is **not** expected: a driver of a real GPU in 3.0, 3D, Vulkan, or a
promise of any of them.

## The process of a major

* **A deprecation is announced in the 2.x line**, with a shim that keeps the old
  behaviour and a note in the release, one minor before it is removed. 3.0
  removes it, and `MIGRATION.md` says what to do.
* **`3.0.0` is not cut by the autopilot.** It writes the notes, cuts
  `v3.0.0-rc1` and every `rc` after it, runs the ten gates and reports; the tag
  of `3.0.0` waits for the word of the owner of the machine. An `rc` that is
  answered by nothing is not a reason to sit still: the autopilot goes on with
  the debts and the side tasks of the same line, and keeps the candidate and
  the report ready.
* **A gate that is not green is not a release.** The ten gates run on the same
  tree, one after another, on a quiet host, with the table of their answers in
  the notes of the release - the discipline of `tools/release_gate.sh`.

## What this gate does not ask

* **A second architecture**: the ARM port is a release of its own with no date
  (`docs/plan-arm64.md`), and a major is not the place to prove a port nobody
  has booted for a year.
* **A phone**: the plan of the ARM port says it is the last mile, not the
  first.
* **A binary-compatible kernel module**: the modules of this tree are compiled
  in (`kernel/kmod.h`), and a stable ABI for them is a promise for a version
  that has a reason to keep it.
