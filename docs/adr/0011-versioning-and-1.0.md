# ADR 0011 - Versioning and the gate of 1.0

* Status: accepted
* Date: 2026-11-21

## Context

The tree is at 0.5.0-rc1. The versions so far followed the phases: 0.3 was the
first release with ports, 0.4 the programs and the permissions, 0.5 the
protection and the first disk. A 1.0 is not "one more phase": it is the point
where the contracts stop moving and another person can build on them without
reading the log.

The project must be small enough for an ESP32 with 8 MB of flash and a
personal machine with a serial console, and honest enough to say what it is
not: not Linux, not a general-purpose distribution, not an ARM port with an
MMU yet.

## Decision

1. **Below 1.0 the minor number is a phase, not a compatibility promise.**
   0.5, 0.6, 0.7 may break the syscall table, the image format or the volume
   if the migration is written and the release notes say so.
2. **1.0 freezes the contracts.** After `1.0.0` the syscall numbers and their
   meaning, the MOX1 format and the MicroFS volume format change only by
   adding; a breaking change is `2.0.0`.
3. **A release candidate is cut before a major.** `1.0.0-rc1` means every gate
   below is met and only fixes are allowed; at least two candidates and four
   weeks on the board are required before `1.0.0`.
4. **The gate is a document, not a feeling.** [docs/releases/1.0.md](../releases/1.0.md)
   is the checklist; a line is `[x]` only when the code, the test and the
   hardware run agree.

The gate in one paragraph: a stable ABI and two stable formats, a program in
ring 3 that cannot reach the kernel, a process model with pipes and signals, a
crash-consistent filesystem with a second one beside it, a network on both
machines, a safe update path, and a release process with sanitizers, fuzzing,
budgets, documentation and a migration from 0.x.

### What 1.0 does not require

* binary compatibility with Linux or ELF instead of MOX1;
* SMP, multiple cores, a general-purpose scheduler;
* TLS/HTTPS, IPv6, a cloud account;
* more than one uid; one user is the model of the device and is documented;
* an ARM port with an MMU: it is the first feature after 1.0, not its gate;
* OTA as the only way to update: USB and a backup of the volume are a safe
  path for a device on a desk.

## Consequences

**Good**

* A person can write a program against `1.0.0` and expect it to run on
  `1.0.7` and `1.9.0`.
* The releases before it are free to move, and the roadmap can say what each
  minor closes.

**Bad**

* The gate is large: pipes, signals, a second writable filesystem, the higher
  half, NX, fuzzing and the update path are each a project.
* A frozen format is a format with a migration tool and a test, not a file
  that happens to be stable today.
