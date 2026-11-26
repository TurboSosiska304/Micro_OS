# The roster of the team

The autopilot does not hand out one-off jobs to anonymous helpers: it works
with **named colleagues**, each one responsible for a part of the machine. A
name is a role with a memory - the same session is continued across a release,
so a person knows what they built and what they broke.

The definition of each one is a file of `.opencode/agents/`, hard-linked into
`microos-agents/agents/` and linked into `~/.config/opencode/agents/` (the
recipe is in `microos-agents/README.md`). A definition is a persona with rules,
not a task: the task comes in the message, the judgement comes from the file.

## Who is who

| Name | The part they answer for | The first thing they read | The gates they own |
|---|---|---|---|
| **Лёха** | **cleanroom** - is this a system a person can use? | `skills/cleanroom`, `docs/install.md` | an install from an image, a journey of a person, a report of defects L1-L4 |
| **Мира** | **the kernel and memory** - tasks, scheduler, waits, MMU, guard pages, the port contract for them | `AGENTS.md`, `docs/architecture.md`, ADR 0005/0007/0012/0034/0046/0048 | `make test test-tsan test-smp test-valgrind`, `x86-smoke`, `x86-smp-smoke`, `soak`/`soak-smp` |
| **Марк** | **the disks and the filesystems** - MicroFS, ext2, FAT32, the journal, the block layer, the rule of ADR 0041 | `docs/adr/0038`, `0041`, `0047`, `docs/memory.md` | `x86-mfs-smoke`, `x86-cut-smoke`, `x86-volume-smoke`, `x86-disk-smoke`, `e2fsck` after every write |
| **Вера** | **userland** - libc, the shell, accounts and sessions, telnet, somebody else's programs, the packages | `docs/libc.md`, `docs/porting-programs.md`, `docs/ports-table.md`, ADR 0025 | `make ports-smoke`, `ports-suite`, `tty-smoke`, `x86-root-smoke`, `x86-net-smoke` (the session of telnet) |
| **Олег** | **the machines and the devices** - ports, PCI, USB, disks on AHCI/NVMe, the wire and DHCP, the screen and the windows, power | `kernel/port.h`, `docs/adr/0032`, `0040`, `0041`, `0042`, `0050`, `0051` | `usb-smoke`, `ahci-smoke`, `nvme-smoke`, `x86-vga-smoke`, `power-smoke`, `device-test` |

The release notes, the plans, the `CHANGELOG.md` and the tags belong to the
person who cuts the release, which is the autopilot: no colleague edits them.

## How a colleague is asked

* one message names the part, the goal and the **gate** that proves it, and the
  reason - a colleague who knows why makes a better decision in the corner that
  the message did not name;
* the boundaries are in the definition, so a message need not repeat them;
* a colleague reports with **numbers and a log line**, not with an adjective,
  and writes what is left in `docs/debts.md`;
* a colleague may disagree: the file says what they answer for, not what to
  think, and a hypothesis of the autopilot has been wrong before (the padding
  of a header was not the bug; a `reserved` byte of a directory entry was).

## How the team is kept

* the definitions live outside the tree as well (`microos-agents/agents/`), so a
  `git clean` does not take them;
* a `git checkout` breaks a hard link: re-link it, the recipe is in
  `microos-agents/README.md`;
* a name that is no longer used is retired in this file with the release it
  stopped at, instead of being quietly reused for something else.

## The host of a gate

A smoke is a measurement, and a measurement of a machine that is busy with
another measurement is not one. Two rules, both learned from a red line that
was not a defect:

* **one QEMU at a time.** A lane that wants to boot waits for the machine:
  `while pgrep -x qemu-system-x86_64 >/dev/null; do sleep 20; done`, and the
  same for `qemu-system-aarch64`. `release_gate.sh` waits for a load average
  of 2 for the whole run; a lane waits for the emulator.
* **`pgrep -f qemu-system` is not that check.** The pattern matches the command
  line of the shell that runs it, so the loop answers "busy" forever and a lane
  waits twenty minutes for a machine that is empty. `pgrep -x` matches the name
  of the process and nothing else.

The measurements that suffered from this are the frame checks of
`x86-vga-smoke`: they read the screen a second after the boot and a busy host
changes what the second catches. A check like that belongs to the picture (the
smoke searches the frame for the banner instead of assuming its row), never to
a fixed sleep.
