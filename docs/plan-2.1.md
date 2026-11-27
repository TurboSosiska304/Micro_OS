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
6. **The MAC and the USB of a real laptop.** K24 accepts a derived locally
   administered address only with a log line that says the station did not come
   from the NVM and an override in `/etc/network/mac`; K25 makes the xHCI reset
   follow the specification and records the answer of the laptop (ADR 0040).
7. **The quality debts that make a report reproducible.** The host account for
   ramfs runs under ASan, TSan and valgrind instead of skipping. K22 writes a
   whole board-console line without blocking, with an SMP test and a soak as its
   proof.
8. **Phase A of the software rasterizer.** Damage is coalesced at the device,
   glyphs come from an atlas, fills and copies use word operations, and a RAM
   back buffer is presented with one batched blit per frame. The pixel and
   fallback gates are those of `docs/plan-gpu.md`. Virtio-gpu remains an opt-in
   experiment and is not part of the release gate. Intel display phase C and AMD
   copy-engine work are not in 2.1.
9. **The eight-core SMP machine and the clean S5.** K27 is in 2.1: its root cause
   is not named yet and `feature/smp-default` is preserved as the starting point.
   K28 is also in 2.1: the defect still reproduces on the owner's laptop, and one
   run of that machine is required to verify the fix (ADR 0012, ADR 0050).
10. **The cleanroom pass.** An independent agent-user exercises the release in
    the simulator and QEMU and returns reproduced L1-L4 findings before the tag.
    The report is `docs/cleanroom/2.1.md` and ships with the release as evidence.
    The procedure and the acceptance rule are below.

### The GPU line

Phase A of [`docs/plan-gpu.md`](plan-gpu.md) is in 2.1: coalesced damage, a
bounded glyph atlas, word copies, a RAM back buffer and one batched blit per
frame. Phase 0 supplies the separate composition, write-combining present and
GOP conversion numbers; phase A must meet its `wm` <= 4 ms QEMU and full present
<= 8 ms laptop targets without changing a pixel.

Virtio-gpu, phase B, is an opt-in experiment run by `make gpu-smoke`. It is not
part of `tools/release_gate.sh` and a failed or absent device backend does not
block the release. The phase A fallback must remain green. Intel display phase C
and AMD copy-engine work are not in 2.1; the read-only probes are reconnaissance,
not a display driver.

## What is not in this release

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
K25, K26, K27 and K28 all belong to 2.1. The numbers below are the identifiers
for this plan: K22 and K25 already exist in `docs/debts.md`; K26-K28 are the three
new entries named there by their file.

* **K22** - board console lines split under two tasks. The port and the device
  test own the answer.
* **K26** - ramfs skips the sanitizers. The host test and the host port own the
  answer.
* **K27** - the eight-core SMP shell can hang. The port, the scheduler and the
  soak own the answer.
* **K28** - S5 can fault and leave the laptop on. The ACPI path and the power
  smoke own the answer.
* **K25** - the xHCI ports do not enumerate. The USB driver, its port test and
  the laptop log own the answer.
* **K24** - the station address of the card. The driver, its log and the
  `/etc/network/mac` override own the answer.

### K22 - the line of the board

`components/port_esp32/src/port_esp32.c` (`k_port_write`) uses a non-blocking
whole-line write. A short critical section copies the bytes into the ring and
drains the complete lines already in it; the path never waits for a sleeping
receiver. An SMP test proves that two writers cannot split a line, and the soak
proves that the ring still drains. Two device-test boots must print whole lines.

### K26 - ramfs under the instruments

`tests/cases/test_ramfs.c` and `ports/port_host/src/port_host.c`
(`k_port_heap_free`) count the allocations and releases of the host itself.
`df`, `ENOSPC` and `mfetch` report that account under ASan, TSan and valgrind,
and the ramfs cases have no sanitizer skip.

### K27 - the hang of eight cores

`ports/port_x86_64/src/port_x86_64_task.c`, `components/kernel/src/sched.c`,
`tools/x86_smp_smoke.py` and `tools/soak.py` own the failing workload. Its root
cause is not named yet. `feature/smp-default` is preserved as the branch that
turns on the shipped eight-core machine and records the attempts to isolate the
hang; it is evidence, not a fix. The lane must leave one transcript and one cause,
then pass `x86-smp-smoke` at 76 checks, the migration stress and both 600-second
soaks with strict console checks restored.

### K28 - the laptop that does not switch off

`ports/port_x86_64/src/acpi.c`, `src/cpu.c`, `src/e1000e.c` and
`tools/x86_power_smoke.py` own the clean end. The defect still reproduces on the
owner's X1 Carbon. The fix must pass `power-smoke` and the `acpi=off` negative,
then one run on that laptop must reach S5 without the page fault and leave the
machine off. ADR 0050 records the DSDT `_S5` decision; QEMU alone cannot close
this debt.

### K24 - the station address of the card

A derived locally administered MAC is acceptable when the I219 NVM does not
answer, but only with both safeguards: the log must say that the station address
did not come from the NVM, and `/etc/network/mac` must be able to override it.
The fabricated `0e:0e:0e:0e:0e:0e` is not an acceptable fallback. The driver and a
negative test must prove the log line, the override and the absence of the old
fabricated address.

### K25 - the ports of xHCI

`ports/port_x86_64/src/xhci.c`, `tests/cases/test_xhci_portsc.c` and
`tools/x86_usb_smoke.py` prove the specified reset on USB 2 and USB 3. The X1
log shows PED with the link in U0 or a named failure; QEMU alone does not close
the row.

K23 is a release blocker: it is closed by the workflow and its artifacts, not
by a warning in a green run. K24 is the MAC decision above. K21 (the console
under a loaded host) remains open unless a loaded-host probe finds and fixes the
cause; a quiet rerun is not a closure. The TSan race in `test_smp_double_claim`
(`tests/cases/test_smp.c`) is part of the K27 work in 2.1; it is not closed by
excluding the test from the detector.

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
5. **The new slices and phase A:** `make tty-pgrp-smoke`, `make log-smoke` and
   the phase A pixel, fallback and benchmark gates. `make gpu-smoke` is the
   opt-in virtio-gpu experiment and is deliberately absent from this release
   list. The new targets do not exist on the base tree yet; adding them is part
   of the lanes below.
6. **The board and the campaign:** `make idf-build`,
   `tools/device-test.sh /dev/ttyACM0`, `make soak` and `make soak-smp` (600
   seconds each). The board test is the K22 hardware gate. `soak-smp` must run
   the shipped eight-core image, not the two-core regression image.
7. **The live power check:** the same signed candidate is booted on the owner's
   X1 Carbon and `poweroff` must reach S5 without the page fault and leave the
   machine off. The report names the image hash and carries the last lines of
   the console. This K28 check is part of the release gate and cannot be replaced
   by `power-smoke` in QEMU.
8. **The repository and the image:** `make reproducible`,
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
* **`gpu-smoke`** is opt-in and is not a release gate. When phase B is exercised,
   it draws one full frame and one damaged frame through virtio-gpu, compares the
   pixels with the software reference, checks that a backend failure falls back
   without losing the console, and compares the same scene's timing with phase A.
   The thresholds are those of `docs/plan-gpu.md`: the device frame is no slower
   than A and small damage adds at most 1 ms. No number is inferred from a byte
   count.

The release tag is cut only after these gates and the cleanroom procedure are
complete. A gate that is not green is not a release; a missing gate is not a pass.

## The cleanroom pass

The pass is a release procedure, not a claim that the machine already works for
an independent user. It runs after the candidate has passed the command gates
and before the release tag.

1. **Who runs it.** A separate agent session receives the candidate image or its
   build, the machine description and the public documentation. It does not read
   the kernel sources or receive the author's explanation of the internals. It
   runs in both the simulator and QEMU, never on the laptop. Its starting
   material is `README.md`, `docs/install.md`, `docs/wiki/`,
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
5. **The gate.** The report is `docs/cleanroom/2.1.md` and is committed with the
   release evidence. The pass is complete when it has run in both the simulator
   and QEMU and every L1/L2 is either fixed and rerun or explicitly recorded in
   `docs/debts.md` with the reason it remains. Only L4 may remain in the release
   without such an entry. An L3 without a debt number does not pass the gate.

## The order of work

The order keeps the cleanup ahead of the experiments. A lane may advance only
when its own gate and the inherited gates are green on its merge base.

1. **C23 and bootlog.** Merge `feature/c23`, then `feature/bootlog`; add
   `log-smoke` and run the host, sanitizer and x86-64 gates. The log must not
   make a boot wait for a slow volume.
2. **The tty slice.** Merge `feature/tty-pgrp`; document calls 52/53 and the two
   ioctls in `docs/syscall-abi.md`; add `tty-pgrp-smoke`; keep `tty-smoke` and
   the pipeline cases green.
3. **K22, K24, K26 and K23.** Close the board line with the non-blocking ring
   write and its SMP test and soak. Implement the MAC decision with its log line
   and override, and close the host memory account. Then make the release
   workflow fail when an artifact is missing. Run the board device test and
   build both media from the candidate.
4. **K25.** Merge the specified xHCI reset with its negative tests. Run
   `usb-smoke`; collect the X1 log that decides the remaining hardware question.
5. **Phase A and the optional probes.** Implement the software rasterizer and
   its pixel, fallback and benchmark gates. Rebase the Intel probe onto the
   shared GPU contract and merge the probes as read-only reconnaissance. Run
   `gpu-smoke` only as the opt-in virtio-gpu experiment; it does not enter
   `tools/release_gate.sh`.
6. **K27 and K28.** Diagnose the eight-core hang from the preserved
   `feature/smp-default`, name its cause, and pass the eight-core smoke and soak.
   Fix the S5 path, pass `power-smoke`, and verify `poweroff` in one run on the
   owner's X1 Carbon. Both debts belong to this release.
7. **The candidate and the cleanroom.** Build the signed candidate, run the full
   command gate and freeze the image hash. Run the independent cleanroom pass in
   the simulator and QEMU, commit `docs/cleanroom/2.1.md`, close or record its
   findings as the procedure requires, and run the final integrity check before
   the tag.

## The owner decided

1. **GPU.** Phase A is in 2.1: damage, a glyph atlas, word copies and a RAM back
   buffer with one blit per frame. Virtio-gpu is opt-in through `make gpu-smoke`
   and is not in the release gate. Intel display phase C and AMD copy-engine
   work are not in 2.1.
2. **Cleanroom.** The independent pass runs in the simulator and QEMU, not on the
   laptop. `docs/cleanroom/2.1.md` is committed and ships with the release as
   evidence.
3. **K24.** A derived locally administered MAC is acceptable only with a log line
   saying that the station did not come from the NVM and an override in
   `/etc/network/mac`.
4. **K22.** The port writes a whole line without blocking: a short critical
   section copies into the ring and drains complete lines, never through a
   sleeping receiver. An SMP test and the soak are the proof.
5. **K27 and K28.** Both are in 2.1. K27 starts from the preserved
   `feature/smp-default` with its root cause still unnamed. K28 still reproduces
   on the owner's laptop and needs one run there to verify the fix.
