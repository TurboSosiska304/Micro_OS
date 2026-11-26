# The autopilot

The owner's instruction of 27 November 2026: the machine runs the ladder on its
own, and the reports stay in the tree and in the chat, **not in Telegram**.

## The ladder

1. **2.1 green.** The plan is [`docs/plan-2.1.md`](plan-2.1.md): C23, the log of
   the machine, the tty and the foreground group, K25, the GPU handshake, K22,
   K26-K30, phase A of the GPU, and the cleanroom. Green means every gate of
   that plan passes on one tree, on a quiet host, in the order of the release
   gate, and `mainline` carries it.
2. **2.2 green, if the machine gets there.** "Eternal Peace"
   ([`docs/release-names.md`](release-names.md)): a page cache a program can
   share, the second half of the journal, `fsync`, the rest of the shell (lists,
   `Ctrl+Z`, `fg`, `bg`) and a pty with a real terminal in the window.
3. **2.3 started.** "Pompeii": fault injection, a night of fuzzing, the errors a
   disk and an allocator make, and a machine that keeps its files. Started means
   the plan is written and the first lane is in work - not that it ships.

## The loop

* one lane, one worktree, one branch, from the tip of `release_candidate`;
* the brief names the files, the constraints, the gate that decides "done" and
  the numbers a report must carry;
* the lane commits often and reports once; a lane that walks into a defect
  outside its area names it and stops there;
* **the merge is mine**: host gates (`make test`, `make integrity`,
  `make x86-build`) and the smokes on the merged tree, one QEMU at a time;
* a branch that breaks the tree is reverted at once and sent back with the
  failing check quoted - `release_candidate` is never red overnight;
* `mainline` is merged at a milestone, and only from a tree whose gate is green.

## The rules that keep the machine honest

* **no Telegram**: the report is a file and a message in the chat;
* one emulator per machine; a lane starts it and takes it down in the same
  command; nothing waits in a background wrapper (`docs/agents.md`);
* a check that fails under load is a check to fix, not to retry until green;
* a defect found on the hardware of the owner goes into `docs/debts.md` with a
  number, the way K29 and K30 did;
* the owner's decisions are recorded, not re-asked: the five answers of 2.1 are
  in `docs/plan-2.1.md`.

## The queue (27 November, night)

| Lane | Branch | State |
|---|---|---|
| the crash that takes the shell and init | `feature/prog-crash` | in work (L1, from the owner's laptop) |
| `rm -r` of a large directory hangs | `feature/k29-cleanroom` | in work (K29 follow-up) |
| the shell that stops answering on eight cores | `feature/smp-hang` | in work (K27, UART-spin hypothesis) |
| phase A of the GPU | `feature/fb-raster` | 284/284 on its branch; merge after the two above |
| 2.2: page cache, journal, `fsync` | not started | after 2.1 is green |
| 2.3: fault injection, fuzzing | not started | the plan first |

## The GPU: from the handshake to the first operation

The handshake of 2.1 answers "what is this card": the vendor and device, the
revision, the BDF, the register window, and on Intel the display version and
the panel timing. What a person wants next, and what each answer costs:

| Question | Today | What it needs |
|---|---|---|
| How much video memory | **AMD: yes** - the size of BAR0 is the aperture (8 GiB on the RX 570), read by the standard BAR size probe. Intel: the aperture of the GTT (256 MiB), the memory itself is shared with the RAM | the exact usable size of the VRAM comes from the MC registers (AMD) or the `GEN9_MEMORY` of the fuse (Intel) - read-only registers, a small lane |
| Which architecture | **yes** - from the ID table and the revision: Polaris 10/20, GCN 4; Intel gen 9.00 stepping C0 | nothing |
| The clock of the chip | no | AMD: the SMU mailbox (firmware); Intel: the PCU mailbox - both are a protocol, not a register |
| Temperature, fans | no | the SMU of AMD, the EC of the laptop - firmware again |
| Read and write the video memory | no | AMD: the aperture at BAR0 is mapped; a test that writes a pattern into a scratch area is a real operation and needs a plan for the scanout that lives there. Intel: the GTT page tables, a real map |
| Draw on the GPU (fill, copy) | no | phase B in QEMU (`virtio-gpu`, safe), then AMD SDMA (firmware + ring), then Intel |
| Everything (3D) | no | a driver of the size of i915; not a release of this year |

The honest boundary: **two of the four questions are answered already** (memory
and architecture), the clocks are behind firmware, and the first *operation*
on a real card - a fill or a copy - is phase B on the emulator first, because a
mistake in the ring of a laptop is a black screen, and a mistake in QEMU is a
restart.
