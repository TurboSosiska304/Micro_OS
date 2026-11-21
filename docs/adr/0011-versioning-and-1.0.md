# ADR 0011 - Versioning and the gate of 1.0

* Status: accepted, with the gate amended twice
* Date: 2026-11-21
* Updated: 2026-11-22 - multi-core and the safe update with a rollback are
  after `1.0.0` (ADR 0012, and the note below); the checksum and the
  signature that the boot checks stay in the gate.


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
machines, a kernel that is correct on one core, a boot that checks the
checksum and the signature of an image, a release process with sanitizers,
fuzzing, budgets and documentation, and a migration from 0.x.

Two lines left the gate on 2026-11-22, and both are written down rather than
dropped: the kernel on more than one core (ADR 0012) and the safe update with
a rollback. A correct single-core system is what 1.0 promises, and a device on
a desk is updated over USB with a backup of its volume until the update path
of 1.0.x arrives; neither changes an ABI, a format or a default of 1.0. The
signature of an image and the checksum the boot verifies are still the gate.

### What 1.0 does not require

* binary compatibility with Linux or ELF instead of MOX1;
* the kernel on more than one core: the second core is 1.x (ADR 0012);
* TLS/HTTPS, IPv6, a cloud account;
* more than one uid; one user is the model of the device and is documented;
* an ARM port with an MMU: it is the first feature after 1.0, not its gate;
* a safe update with a rollback: it is the first block after 1.0.0; a device
  on a desk is updated over USB with a backup of the volume until then.

## Consequences

**Good**

* A person can write a program against `1.0.0` and expect it to run on
  `1.0.7` and `1.9.0`.
* The releases before it are free to move, and the roadmap can say what each
  minor closes.

**Bad**

* The gate is large: pipes, signals, a second writable filesystem, the higher
  half, NX, fuzzing and the signature of an image are each a project; the
  second core and the update path are the first projects after the release.
* A frozen format is a format with a migration tool and a test, not a file
  that happens to be stable today.
