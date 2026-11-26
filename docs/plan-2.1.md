# The plan of 2.1 - Rooks

2.1 is not a new set of promises. It is the cleanup after 2.0: the defects the
major left, the debts the machine of its user exposed, and the work that was
already meant for this number but missed the patch. It also requires a cleanroom
pass: an independent agent uses the system as a person would and brings back a
list of reproduced logical defects. The name and the promise are in
[`docs/release-names.md`](release-names.md); this document is the list of lanes,
numbers and decisions, not a date.

Related: [`docs/roadmap-to-3.0.md`](roadmap-to-3.0.md) 10a-10b,
[`docs/debts.md`](debts.md), [`docs/plan-2.0.2.md`](plan-2.0.2.md),
[`docs/plan-gpu.md`](plan-gpu.md), ADR 0011 (the contract of a version),
ADR 0012 (many cores), ADR 0040 (USB), ADR 0050 (the clean end),
ADR 0051 (the terminal), ADR 0058 (the language).

## What is already in work

The state below is the state of the worktrees on 26 November 2026. Code on a
branch is not a merge, and a test of a part is not a gate of the release.
`docs/plan-gpu.md` lives on `feature/gpu-plan`; its links here name that branch
until the plan is merged.

* **`feature/c23`.** The host and x86-64 builds select C23, extensions are off
  by default, and only targets that need POSIX opt into `gnu23` themselves.
  The code exists. The full merge gate - tests, sanitizers, firmware and the
  x86-64 smokes - still has to run. The board keeps the dialect of ESP-IDF 5.3;
  the common source stays in the subset both compilers accept. The decision is
  ADR 0058 and the scope is
  [`docs/roadmap-to-3.0.md`](roadmap-to-3.0.md), 10b.
* **`feature/bootlog`.** `components/kernel/src/logfile.c` writes
  `/etc/bootlog.log`, `bootlog.1.log`, `run.log` and `run.1.log`, bounded at
  64 KiB each. The console hook only copies bytes; a service task writes them,
  and a read-only root keeps two generations in RAM. The password prompts are
  hidden, including on the session of telnet. The code and its host tests exist.
  The QEMU disk/stick log smoke and the device test still have to pass on the
  merged tree. This is 10a of the roadmap.
* **`feature/tty-pgrp`.** A tty has a foreground process group, `getpgid` and
  `setpgid` are ABI calls 52 and 53, and `TIOCGPGRP`/`TIOCSPGRP` name the group.
  A pipeline runs in one group and Ctrl+C reaches that group. This is the small
  slice: no Ctrl+Z, no `SIGTSTP`, no `jobs`, `fg`, `bg`, `&` or background wait
  flags. The working tree has code and host tests, not a finished QEMU smoke;
  the shell also has to keep its existing wait and pipeline gates green.
* **`feature/xhci-k25`.** `xhci.c` powers a port when the controller supports
  it, waits for the power, clears the change bits before the reset, waits for
  PRC and then PED with a USB 3.0 link in U0, and retries the whole sequence
  five times with a delay. `tests/cases/test_xhci_portsc.c` and the USB 3 case
  of `tools/x86_usb_smoke.py` exist. The code is on the branch; the QEMU smokes
  and the log of the owner's X1 Carbon still have to answer. ADR 0040 owns the
  bus, and K25 in `docs/debts.md` owns the remaining laptop evidence.
* **`feature/gpu-amd` and `feature/gpu-intel`.** The two branches identify a
  display function and read a small, read-only set of registers. They do not
  set a mode, submit a command or draw a frame. The shared probe lives behind
  `ports/port_x86_64/include/port_x86_64_gpu.h` on the AMD branch; the Intel
  branch has to adopt that interface before `feature/gpu-merge`.
  `docs/ports/x86/gpu-amd-recon.md` and `gpu-intel-recon.md` record what the
  probes saw. `feature/gpu-merge` is still at the base, so there is no merged
  GPU code and no `gpu-smoke` yet.

## What 2.1 contains

1. **C23 for the host and x86-64.** One language baseline for the tree and its
   tests; the board remains on ESP-IDF's dialect (ADR 0058, 10b).
2. **A log a report can carry.** The boot capture and the console transcript
   reach `/etc` on a writable root; passwords never reach either log, and a
   read-only root says that its log is in RAM (10a).
3. **A tty with a foreground group.** Ctrl+C reaches the pipeline that owns the
   terminal, while a background session cannot take the keyboard of another
   session (ADR 0051). Full job control is not part of this slice.
4. **The desktop and the media defects of 2.0.** The read-only root and its
   ramfs, the desktop on the ISO and stick, and the framebuffer policy stay
   release requirements, not branches to re-open
   ([`docs/plan-2.0.2.md`](plan-2.0.2.md)).
5. **The release medium built by the runner.** K23 ends when the workflow builds
   or obtains its own `limine`, fails on a missing ISO or stick, and publishes
   the assets it actually booted.
6. **The MAC and the USB of a real laptop.** K24 replaces the fabricated
   `0e:0e:0e:0e:0e:0e` with the station of the card or a derived address that
   says it is derived; K25 makes the xHCI reset follow the specification and
   records the answer of the laptop (ADR 0040).
7. **The quality debts that make a report reproducible.** The host account for
   ramfs runs under ASan, TSan and valgrind instead of skipping, and the console
   of the board keeps a whole line under two tasks. SMP hang diagnosis runs on
   the 2.0.3 line in parallel.
8. **A read-only GPU reconnaissance.** The AMD and Intel probes and their notes
   may land as observation, not as a display driver. They must not change a
   mode, take the screen or become a new boot dependency.
9. **The cleanroom pass.** An independent agent-user exercises the release and
   returns reproduced L1-L4 findings before the tag. The procedure and the
   acceptance rule are below, not a claim that the pass has already happened.

### The GPU line is a decision, not a promise

[`docs/plan-gpu.md`](plan-gpu.md) recommends phase 0 and phase A: measure RAM
composition, the write-combining present and GOP conversion separately, then
optimize the software rasterizer. Phase A is a candidate for 2.1 **only if the
owner approves it**. It is not approved by this plan.

The approval must say whether phase A is in the release, and whether virtio-gpu
(phase B) is only an opt-in experiment or a required gate. If A is not
approved, 2.1 still has the probes and the software path of 2.0.2; there is no
partial rasterizer in the release. If B is approved, `gpu-smoke` compares the
device frame with the software reference and fails if the device path is slower
than that reference. A real Intel or AMD display driver is not in 2.1.

## What is not in this release

* **SMP on eight cores.** 2.0.2 ships one core because the soak can stop
  answering on eight. The hang, the migration stress and the strict
  `test_smp_double_claim` locking are 2.0.3 work, not a 2.1 feature lane. The
  existing two-core `x86-smp-smoke` remains a regression gate (ADR 0012).
* **A laptop that switches itself off.** The S5 page fault after the write and
  the real `_S5` sleep type are 2.0.3 work. 2.1 keeps `power-smoke` green; it
  does not claim the live laptop is fixed (ADR 0050).
* **A full `dash` or `bash`, and full job control.** The foreground group and
  Ctrl+C are the 2.1 slice. The lists, redirections, variables, background jobs,
  Ctrl+Z, `fg`, `bg` and a userland `/bin/sh` are 2.2 work (ADR 0017).
* **A pty and a real terminal in a desktop window.** That needs its own ABI and
  session model; the tty slice does not turn the two pipes of `/bin/term` into
  a controlling terminal (ADR 0051).
* **A GPU that drives the laptop.** No Intel mode setting, no AMD command ring,
  no 3D, no Vulkan and no loadable driver package. The host experiments and the
  probes do not change that boundary.
* **Durability and the next major.** A page cache, copy-on-write of a file
  chain, `fsync`, a second half of the journal, packages and an upgrade across a
  major belong to 2.2 or 3.0 ([`docs/roadmap-to-3.0.md`](roadmap-to-3.0.md)).

## The debts of 2.1

A debt closes with code and a test, and its row moves to the release notes. K22,
K25 and K26 belong to 2.1. K27 and K28 are recorded here so that this plan names
the known holes, but both remain on the 2.0.3 line. The numbers below are the
identifiers for this plan: K22 and K25 already exist in `docs/debts.md`; K26-K28
are the three new entries named there by their file.

* **K22** - board console lines split under two tasks. The port and the device
  test own the answer.
* **K26** - ramfs skips the sanitizers. The host test and the host port own the
  answer.
* **K27** - the eight-core SMP shell can hang. The port, the scheduler and the
  soak own the answer. This stays on the 2.0.3 line.
* **K28** - S5 can fault and leave the laptop on. The ACPI path and the power
  smoke own the answer. This also stays on the 2.0.3 line.
* **K25** - the xHCI ports do not enumerate. The USB driver, its port test and
  the laptop log own the answer.

### K22 - the line of the board

`components/port_esp32/src/port_esp32.c` (`k_port_write`) takes one non-blocking
critical section for the whole UART buffer. A host test of the port path and two
device-test boots produce whole lines. No lock is added to a path that can sleep.

### K26 - ramfs under the instruments

`tests/cases/test_ramfs.c` and `ports/port_host/src/port_host.c`
(`k_port_heap_free`) count the allocations and releases of the host itself.
`df`, `ENOSPC` and `mfetch` report that account under ASan, TSan and valgrind,
and the ramfs cases have no sanitizer skip.

### K27 - the hang of eight cores

`ports/port_x86_64/src/port_x86_64_task.c`, `components/kernel/src/sched.c`,
`tools/x86_smp_smoke.py` and `tools/soak.py` own the failing workload. It leaves
one transcript and one cause; the fix passes `x86-smp-smoke`, the migration
stress and both 600-second soaks. Strict console checks are restored, not
relaxed. This stays on the 2.0.3 line.

### K28 - the laptop that does not switch off

`ports/port_x86_64/src/acpi.c`, `src/cpu.c`, `src/e1000e.c` and
`tools/x86_power_smoke.py` own the clean end. It reaches S5 without a fault on
the owner's X1 Carbon, or a named remaining cause belongs to 2.0.3.
`power-smoke` and the `acpi=off` negative stay green. ADR 0050 records the DSDT
`_S5` decision. This also stays on the 2.0.3 line.

### K25 - the ports of xHCI

`ports/port_x86_64/src/xhci.c`, `tests/cases/test_xhci_portsc.c` and
`tools/x86_usb_smoke.py` prove the specified reset on USB 2 and USB 3. The X1
log shows PED with the link in U0 or a named failure; QEMU alone does not close
the row.

K23 and K24 are release blockers even though they are not kernel defects. K23
is closed by the workflow and its artifacts; K24 by the driver path and a test
that refuses the fabricated station address. Their register entries remain in
`docs/debts.md` K23-K24. K21 (the console under a loaded host) remains open unless
a loaded-host probe finds and fixes the cause; a quiet rerun is not a closure.
The TSan race in `test_smp_double_claim` (`tests/cases/test_smp.c`) is fixed on
the 2.0.3 line; it is not closed by excluding the test from the detector.

## The gates of the release

Every gate runs on the same merge, in order, on a quiet host. The list and the
logs live in `tools/release_gate.sh`; a second run that passes is recorded as a
second run. The table of the release notes carries the actual counts, not only
"PASS". A branch that has no smoke has not passed by having unit tests.

### The inherited numbers

These are the counts of 2.0.2, the floor that 2.1 must keep or exceed. The
integrity count is the final repository check, not the count before the release
notes are written.

| Command | Expected checks |
|---|---:|
| `make x86-smoke` | 429 |
| `make x86-vga-smoke` | 284 |
| `make x86-smp-smoke` | 76 |
| `make usb-smoke` | 66 |
| `make uefi-stick-smoke` | 21 |
| `make x86-iso-desk-smoke` | 9 |
| `python3 tools/integrity_check.py --expect-tag v2.1.0` | 47 |

A lower count fails unless the release notes name the removed case and its
replacement. `integrity` is run once without a tag while preparing the plan,
and with the release tag for the release gate; a development checkout has no
release tag and must not invent one.

### The commands

The 2.1 gate is the whole list below, plus every command already in
`tools/release_gate.sh`. `make ci` is the quick development gate, not the
release gate.

1. **The host and the language:** `make test`, `make test-asan`,
   `make test-tsan`, `make test-valgrind`, `make test-smp`, `make test-minimal`
   and `make smoke`. C23 is part of these builds; the ramfs cases run in the
   instrumented builds.
2. **The machine:** `make x86-smoke`, `make x86-vga-smoke`,
   `make x86-smp-smoke`, `make usb-smoke`, `make mmu-smoke`,
   `make fork-smoke`, `make power-smoke`, `make x86-disk-smoke`,
   `make x86-root-smoke`, `make x86-volume-smoke`, `make x86-mfs-smoke` and
   `make x86-cut-smoke`.
3. **The wire and the programs:** `make x86-net-smoke`, `make ports-smoke`,
   `make tty-smoke`, `make ahci-smoke` and `make nvme-smoke`.
4. **The media:** `make uefi-smoke`, `make uefi-stick-smoke`,
   `make x86-iso-smoke`, `make x86-iso-uefi-smoke`, `make x86-iso-usb-smoke` and
   `make x86-iso-desk-smoke`. The ISO and stick smokes boot the artifacts the
   bundle publishes, not a substitute disk image.
5. **The new slices:** `make tty-pgrp-smoke`, `make log-smoke` and, if the owner
   approves the device backend, `make gpu-smoke`. The targets do not exist on
   the base tree yet; adding them is part of the lanes below.
6. **The board and the campaign:** `make idf-build`,
   `tools/device-test.sh /dev/ttyACM0`, `make soak` and `make soak-smp` (600
   seconds each). The board test is the K22 hardware gate. The two-core soak is
   mandatory; the eight-core soak is evidence for 2.0.3, not a 2.1 promise.
7. **The repository and the image:** `make reproducible`,
   `make integrity` and a clean Release build from an empty directory. The
   final call is
   `python3 tools/integrity_check.py --expect-tag v2.1.0`.

### What the new smokes must prove

* **`tty-pgrp-smoke`** starts a pipeline and an unrelated background session,
  checks `TIOCGPGRP`/`TIOCSPGRP` and calls 52/53, sends Ctrl+C, and proves that
  only the foreground group receives SIGINT and that the prompt returns. It
  checks the negative too: the other session keeps running. No case claims
  job suspension or background execution.
* **`log-smoke`** boots a writable x86-64 image and the read-only ISO. It proves
  that the boot capture becomes `/etc/bootlog.log`, the previous attempt becomes
  `bootlog.1.log`, `run.log` has the session transcript, both files stay within
  64 KiB, and rotation says that it happened. It types a password at `login`,
  `su` or `passwd` and proves that neither log contains it. The ISO must say
  that its log is in RAM. The device test proves the board's RAM fallback.
* **`gpu-smoke`**, only after approval, draws one full frame and one damaged
  frame through virtio-gpu, compares the pixels with the software reference,
  checks a backend failure falls back without losing the console, and compares
  the same scene's timing with phase A. The thresholds are those of
  `docs/plan-gpu.md`: the device frame is no slower than A and small damage adds
  at most 1 ms. No number is inferred from a byte count.

The release tag is cut only after these gates and the cleanroom procedure are
complete. A gate that is not green is not a release; a missing gate is not a pass.

## The cleanroom pass

The pass is a release procedure, not a promise that the machine already works
for a stranger. It runs after the candidate has passed the command gates and
before the release tag.

1. **Who runs it.** A separate agent session receives the candidate image or its
   build, the machine description and the public documentation. It does not read
   the kernel sources or receive the author's explanation of the internals. Its
   starting material is `README.md`, `docs/install.md`, `docs/wiki/`,
   `commands.md`, the messages of the machine and the ADR that names a documented
   limitation. If only the source can explain the intended behaviour, that is a
   documentation finding.
2. **What it does.** The agent installs or boots the image and works through the
   user journeys: create, write, copy, rename and remove files; run programs and
   pipelines; use the network when the build provides it; open the desktop and a
   terminal; read and change the settings; and reboot to find what survives. It
   records two different things: **"not clear"** when the documentation or the
   interface did not tell it what to do, and **"broken"** when a demonstrated
   result contradicts the documentation or loses the task.
3. **What it returns.** One report names the candidate commit and image hash, the
   machine and medium, the journeys, the exact commands, the expected answer,
   the observed answer, `/etc/bootlog.log`, `/etc/run.log` and the transcript
   lines or a screenshot. Every defect has steps that reproduce it and one level:
   * **L1** - crash, data loss, hang or security;
   * **L2** - a wrong result;
   * **L3** - confusing output, a contract that differs from the documentation,
     or an accepted flag that does not do what it says;
   * **L4** - cosmetic.
   The report keeps **"not clear"** separate from **"broken"** and lists
   documented limitations separately, with the ADR or manual section that names
   each one. A **"broken"** entry with no demonstrated failure is **"not clear"**,
   not a defect.
4. **What the release does with it.** L1 and L2 are blockers. An L3 gets a
   numbered entry in `docs/debts.md` and an owner; an L4 is cosmetic and does not
   block the release. A fixed finding needs a regression case and a rerun of its
   steps on the candidate. The author does not close a finding by explaining it
   in chat.
5. **The gate.** The report is stored with the candidate evidence. The pass is
   complete when it has run and every L1/L2 is either fixed and rerun or
   explicitly recorded in `docs/debts.md` with the reason it remains. Only L4
   may remain in the release without such an entry. An L3 without a debt number
   does not pass the gate.

## The order of work

The order keeps the cleanup ahead of the experiments. A lane may advance only
when its own gate and the inherited gates are green on its merge base.

1. **C23 and bootlog.** Merge `feature/c23`, then `feature/bootlog`; add
   `log-smoke` and run the host, sanitizer and x86-64 gates. The log must not
   make a boot wait for a slow volume.
2. **The tty slice.** Merge `feature/tty-pgrp`; document calls 52/53 and the two
   ioctls in `docs/syscall-abi.md`; add `tty-pgrp-smoke`; keep `tty-smoke` and
   the pipeline cases green.
3. **K22, K24, K26 and K23.** Close the board line, the MAC and the host memory
   account. Then make the release workflow fail when an artifact is missing.
   Run the board device test and build both media from the candidate.
4. **K25.** Merge the specified xHCI reset with its negative tests. Run
   `usb-smoke`; collect the X1 log that decides the remaining hardware question.
5. **The GPU merge, if approved.** Rebase the Intel probe onto the shared GPU
   contract and merge the probes. Run all display and boot fallbacks. Phase A
   and virtio-gpu are separate merges with their own gates; neither may hold up
   the cleanup if the owner declines them.
6. **The 2.0.3 line in parallel.** Diagnose K27 and K28 on their own branch. A
   fix may land before 2.1, but 2.1 does not claim eight cores or laptop S5 until
   the patch has its own release and evidence.
7. **The candidate and the cleanroom.** Build the signed candidate, run the
   full command gate and freeze the image hash. Run the independent cleanroom
   pass, close or record its findings as the procedure requires, and run the
   final integrity check before the tag.

## What the owner decides

1. Is GPU phase A in 2.1, and is virtio-gpu an opt-in experiment or a required
   `gpu-smoke`? The recommendation is A first, B only after it beats the
   software reference.
2. Which agent session and machine run the cleanroom pass, and where is its
   report kept with the candidate evidence?
3. Is a locally administered derived MAC acceptable when the I219 NVM is held
   by the management engine, or must the wire stay off until a real station
   address is read?
4. Does the K22 fix have to wait for proof that every `k_port_write()` caller
   can tolerate a lock, or may the lane first prove a non-blocking whole-write
   critical section?
5. Are K27 and K28 being cut as 2.0.3 before 2.1, or may a proven fix join the
   candidate? The release notes must name which answer was taken.
