# Review of the portability work

A map for the review of the changes between `869008b` (the last commit
before the boundary work) and the freeze point. The point of the document
is that a reviewer does not have to reconstruct what happened: every
commit is listed with what it changed, which property it is supposed to
keep, and what was run on it.

The freeze point is the tag `v0.3.0-pre-x86`, made when the x86-64 port
reaches the shell. Reviewing a moving tree wastes the review.

## How to run the checks

```bash
make ci                      # host: unit, ASan, smoke, integrity
make idf-build               # firmware, needs ../esp-idf
tools/device-test.sh /dev/ttyACM0
make x86-smoke               # the kernel on a QEMU x86-64 machine
make sim && make smoke       # the simulator, with a transcript
```

Ceiling of the automated checks, to keep the review honest:

* the host build does not reproduce preemption: the port's critical
  sections are a mutex (ADR 0001), so a race in the kernel is invisible
  there;
* the ThreadSanitizer build exists (`make test-tsan`) and is clean; it is
  not part of `make ci` yet (a build of its own), so a reviewer runs it;
  its first run found five real defects, all fixed before this review;
* `flash0` is only exercised on the board: the tests use a RAM device;
* the x86-64 port is outside `make ci` (it needs QEMU).

## The commits

| # | Commit | Area | The property it must keep |
|---|---|---|---|
| 1 | `f062180` | kernel | the contract is split by subject; the port reports its version and the boot checks it |
| 2 | `7320dda` | docs | ADR 0004 records the boundary and the audit |
| 3 | `7568574` | kernel | upper layers ask `k_sysinfo/k_console/k_reboot/k_mutex`, not the port |
| 4 | `030af44` | kernel, board | the board description is kernel data, registered at boot |
| 5 | `9aa2d9b` | kshell, procfs, devfs | no `k_port_*` above the kernel; `kshell` does not depend on `board` |
| 6 | `35733c4` | mfs, telnet | a subsystem lock is `k_mutex` (blocks, recursive), not a critical section |
| 7 | `aea9ea2` | tools | `[layers]`/`[port]` fail on a deliberate violation |
| 8 | `88df0f0` | kernel | `blockdev` registry, `kv` and `net` as port duties, contract 1.2 |
| 9 | `4621047` | kernel | every concession to a board without an MMU sits behind `KERNEL_NOMMU` |
| 10 | `9e80714` | docs | the locks and the new interfaces are described as they are |
| 11-13 | `5ac08e6`, `d4aeff0`, `2c27f9c` | mfs, config, net | the backends left the portable components; **the firmware does not build until 15** |
| 14 | `fd5b550` | kernel, drivers | the clock is a door (`k_clock_set_unix`), NTP does not run a shell command |
| 15 | `f7dc5ef` | main, tools | the boot mounts `flash0`, installs the store before the restore; the gates follow the tree |
| 16 | `843c216` | wifi | the radio is port code; the core asks the kernel for the lock and the time |
| 17 | `44c14d8` | graphics | the console draws through `k_render`; the panel registers it; `display` is the driver's command |
| 18 | `e73270d` | tools, docs | the platform debt list is empty |
| 19 | `448c606` | port_host | the simulator boots the whole portable system on a file volume |
| 20 | `de09af7` | port_null, tools | the kernel links against a port with no POSIX and no chip |
| 21 | `3f94ca7` | docs | `docs/porting.md` is the guide for the next machine |
| 22 | `a42f8c7` | port_null | formatting of a checkout, not only of a working tree |
| 23 | `b794921` | tests | the concurrency test does not lose an update of its counter |
| 24 | `1ebf940` | port_host | the simulator has the driver stubs, the doctor, the clock and a renderer |
| 25 | `e1b5195` | x86-64 | the kernel boots on a bare-metal x86-64 machine, with its own loader |
| 26 | `7aed53e` | docs | the x86-64 port in the roadmap and the architecture |

Commits 11-13 are the only ones that do not build the firmware on their
own: they move the backends before the wiring commit 15. Review them as
one change.

### The work after the map

The review itself produced commits; they are part of it.

| # | Commit | What it is, and what to look at |
|---|---|---|
| 27 | `f8c535e` | this document |
| 28 | `93261f6` | the race detector build (`make test-tsan`), outside `make ci` |
| 29 | `5bf12db` | the five defects the detector found: the ring of `klog`, the table of the console sinks, `task_exit` after the zombie, the waiters, the state of a task (`task_state_of`) |
| 30-32 | `ed1d0de`, `484e0b0`, `217106b` | the x86-64 port: IDT/PIC/PIT, the context switch and the scheduler, the shell on COM1 |
| 33 | `4e46ee0` | the roadmap and the review document after the two |
| 34 | `63ec82d` | the flash audit's F1, F3, F5: a volume is not formatted on a guess, a damaged header is refused, the tail of the partition is dropped |
| 35 | `eefc56c` | the eight findings of the audit in this document |
| 36-39 | `3d8686a`...`51c3840` | the documents brought to one state (three builds, the applications out, the translations say that they lag, the porting guide counts the fourth port) |

The first of these is the reason the review started early: the race detector
and the audit of the flash were run before the review, found five defects of
the kernel and three ways to lose the data of the user, and they are fixed
with tests passing. A reviewer should still read the diffs: the fixes are
small, but they change locking in the middle of the kernel.

## Risk register, most expensive first

1. **`flash0` and the erase of the flash** (`5ac08e6`,
   `components/port_esp32/src/blockdev_flash.c`).
   The device caches the 4 KB erase sector, writes collect in it, and
   `sync` erases and programs. A mistake loses the volume of the user and
   no test on the host can see it.
   *Audited, and it found a trap:* the mount called a volume fresh when
   its last block was erased, so a partition that grew or moved would be
   formatted at the next boot - the data erased without a word. Fixed in
   `63ec82d`: a volume is fresh only when the whole erase sector is blank,
   a damaged header is refused instead of formatted, and the partition is
   measured in whole 4 KB sectors. What remains structural is below.
   *Prove it:* write a hundred files, `sync`, reboot, compare every byte;
   `mkfs -y`, repeat; pull the cable in the middle of a write and check
   that the volume still mounts or says it is damaged.
2. **The switch of the contexts on x86-64** (new, `switch.S`,
   `port_x86_64_task.c`).
   The frame of a new task, `-mno-red-zone`, a switch from inside the
   timer interrupt. A mistake is a triple fault without a word.
   *Prove it:* two tasks that increment their own counter and print it;
   `ps` shows both; a task that sleeps is woken by the tick.
3. **`k_mutex` with a lazy handle** (`kernel/lock.c`).
   The handle is created outside the critical section and the loser of
   the race destroys its own. Rare and nasty.
   *Prove it:* a test with two tasks that take the same fresh mutex at
   the same moment, many times; ASan on.
4. **Telnet under a lock** (`2c27f9c`, `telnet_server.c`).
   The descriptor is protected, the socket calls are not; `wait=false`
   drops output; `recv` does not tell silence from a dead peer.
   *Prove it:* a session over the loopback on the host, a client that
   stops reading, `telnet off` in the middle of a command.
5. **The store of the configuration** (`d4aeff0`).
   The file backend writes a temporary file and renames it; the NVS
   backend opens the namespace lazily; `config_register()` installs the
   store as a side effect, which the boot depends on.
   *Prove it:* `config sync`, restart, `mkfs -y`, restart; a key with a
   value of 127 bytes and one of 128; a key of 15 and one of 16.
6. **The sockets** (`2c27f9c`). `recv` returns -1 for a timeout and for
   an error; the session timeout of 1 s is the policy of the backend and
   not of the contract.
   *Prove it:* a server that closes, one that answers nothing, one that
   answers slowly.
7. **The console on a renderer** (`44c14d8`). The turn of the panel is
   noticed by comparing the size; `s_demo` silences the renderer; the
   flush task runs every 100 ms.
   *Prove it:* rotate in the middle of a long output; `display draw`
   while the shell prints; unplug the notion of a display (no renderer).
8. **The radio** (`843c216`). It comes up earlier than before (inside
   `port_esp32_init`), the core takes a mutex from the event loop, the
   MAC travels through the kernel.
   *Prove it:* reboot with a saved network; scan during a connection;
   a network that disappears.
9. **The gates** (`aea9ea2`, `e73270d`). They are regular expressions:
   `# include` with a space is caught, `(k_port_write)(...)` is not.
   *Prove it:* try to sneak a violation past each rule.
10. **The tiny memory gate** (`f7dc5ef`). `PORT_IMPLEMENTATION_DIRS` scans
    whole directories; a port split into many files is covered, a call
    made through a function pointer is not.

## Findings of the flash audit

The audit of `flash0` (read-only, before this review) found eight things.
Three are fixed already, because one of them erased data without the user
doing anything; the rest are for this review to decide.

| # | Finding | State |
|---|---|---|
| F1 | a grown or moved partition was "fresh" at its new end, so the boot formatted the volume: silent data loss | **fixed** in `63ec82d` |
| F2 | the unit of loss is the 4 KB erase sector, and the bitmap with the root inode share one sector: a power cut during `sync` can take the root directory and the bitmap together | design: shadow or a layout of its own for the bitmap; document the 4 KB unit |
| F3 | a damaged volume mounted and reported a healthy size, because the root inode was never checked | **fixed** in `63ec82d` (the mount refuses it) |
| F4 | an interrupted `mkfs -y` leaves a volume that mounts with an empty table: valid header, no root | design: erase META first / mark a format in progress |
| F5 | a partition size that is not a multiple of 4 KB leaves a tail the flash refuses to write | **fixed** in `63ec82d` (the tail is dropped with a warning) |
| F6 | one `write` costs ~21 erase sectors (the filesystem jumps META and DATA), which is wear and latency | design: the bitmap in its own erase sector, fewer syncs |
| F7 | the cache of the port has no `invalidate`, so the filesystem cannot say "forget this volume"; the contract does not say the device is not reentrant | add to `kernel/blockdev.h` |
| F8 | `ops->erase` is never called and never tested | decide: tests, or out of the contract |

The recommendation of the audit stands for the review: move the NOR cache
out of `port_esp32` into a portable module and test it against a model of
the flash (a bit can be cleared, only an erase sets it back). Then F2, F4
and F6 stop being "only the board".

The on-device procedure the audit wrote down is the one to run after the
review: write ten files and check the time and the free space; power-cut
the board in the middle of a `write` twenty times and check that the volume
never formats itself; power-cut a `mkfs -y`; and (on a board that may lose
its data) change the size of the partition by one sector and check that the
mount refuses instead of formatting.

## Debts and gaps known before the review

* `memtest` allocates through `kmm`, and `KMM_PRIORITY_KERNEL` may fall
  back to the PSRAM when the internal RAM is short: the report can
  measure the wrong memory (decision D17).
* The session timeout of telnet is a constant of the backend
  (`PORT_NET_SESSION_TIMEOUT_MS`), not of `kernel/net.h`.
* The console through a renderer is compiled on the host but not tested
  there.
* The race detector: closed. The first run reported fifty races; the
  triage separated thirty defects of the tests (counters shared between
  tasks with plain `++`) from five of the kernel, and the five were fixed
  in `5bf12db`: the ring of the log had no lock at all, the table of the
  console sinks was read while another task could replace it (and had no
  removal), a task read its own name and pid after publishing itself as a
  zombie, the waiters read the state of a slot they did not own, and the
  state of a task was read plainly by `ps`, the shell and the parent. The
  state now has the accessor `task_state_of()`, the console has
  `k_console_remove_sink()`, and `make test-tsan` passes.
* The x86-64 port is not part of `make ci`: `make x86-smoke` is a target
  of its own (a reviewer must remember it).
* The kernel keeps no wall clock it can read: `k_clock_*` can set the
  time, nothing can ask for it yet (the file timestamps will need it).

## What a good review of this range produces

1. A list of the bugs, each with the commit that introduced it and the
   test that now covers it.
2. A list of the invariants that the code does not hold, with the gate
   that should have caught it.
3. A decision about the debts above: pay now, or write them down in the
   roadmap with a date.
4. Nothing else. A review is not the place to reformat the tree: the
   formatting is checked by `make integrity`, and a mixed commit hides
   the change that matters.
