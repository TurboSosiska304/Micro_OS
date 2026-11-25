# ADR 0053 - The setuid bit of an image

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0005 (programs and protection), ADR 0006 (the entry of a
  program), ADR 0011 (the ABI of the calls), ADR 0022 (the layout of 1.1),
  ADR 0033 (the accounts, first half), ADR 0044 (the session and the group),
  ADR 0048 (the guard of a stack), `docs/plan-2.0.md` M3,
  `docs/syscall-abi.md`, `docs/debts.md`

## Context

ADR 0033 refused a setuid bit in `MOX1` for three reasons: the format is
frozen, the kernel had one credential and no saved uid, and nothing of the
tree needed the bit. The refusals were recorded with the shape of the work:
a flag in the header, a real/effective/saved uid in `struct task`, the rule of
`pkg` for an image that carries it, and nine bits of `chmod` that do not hold
it.

The release of the group (ADR 0044, the plan of 2.0 M3) took the first half:
a record of a filesystem has an owner and a group, the check reads the three
triads, and `/etc/passwd`, `/etc/shadow` and `/etc/group` name a person. The
one thing a person still cannot do is run a program that needs to read a file
of the machine's accounts - a `passwd` that writes `/etc/shadow`, a `ping`
that opens a raw socket one day - without becoming root for the whole of the
shell. The console has `su` for that, and the point of a session of `telnet`
is precisely that its person has no console to type `su` on.

## The question

May a program ask for the identity of the owner of its file, and what exactly
does the kernel give it, on a machine whose programs share the memory of the
kernel on two of its three ports and run in ring 3 on the third?

## The decision: a flag of the image, three uids in the task, the act of execve

**1. The flag is a bit of the field the header already has.** `MOX1` version 2
has a `flags` word at offset 8, `K_EXEC_FLAG_SETUID` (`0x2`) asks for the
identity of the owner of the file, and `K_EXEC_FLAGS_KNOWN` names the two
flags the format knows (`WX` is still refused: a page both writable and
executable). The version does not move: the field existed, the bit was
unknown before this decision, and an image that set it was refused then too -
so no existing image changes its meaning, and no migration is needed. The
parser copies the bit into `struct k_exec_image::setuid` and decides nothing
else with it.

**2. A task has the three uids of POSIX.** `struct task` keeps `uid` (the
real uid), `euid` (the effective one) and `suid` (the saved one).
`task_get_uid()` answers the **effective** uid, because every check of the
kernel already asked it and every check of the kernel should read it: the
access of an open, the write of a directory, the search of a path, root's
bypass, the signals of another user, the mount of a volume. The real uid is
answered by `getuid` and the saved one by nothing of the ABI; `login`, `su`
and a test move the three together (`task_set_uid`, `task_set_creds`).

`setuid` follows POSIX with these three:

* a task whose effective uid is zero (root) names any uid of the fifteen bits
  of a record and the three uids become that number - this is how a program
  that entered with the identity of its owner gives it up, and how
  `setuid(getuid())` also clears the saved uid so the identity cannot be taken
  back;
* any other task names its real uid or its saved uid and the effective uid
  moves to it; any other number is `EPERM`. The real uid never moves but by
  the hand of root.

There is no saved gid and no `setgid` bit: a setuid image changes no group,
so the gid keeps the one rule it had (a task names its own gid, root names
any). A supplementary group list is a change of `struct task` and of every
entrance, and no program of this tree asks for one (ADR 0044 §4).

**3. The act is the call of execve, and the file has to be the owner's.**
The parser names the flag; `sys_execve` does the rest, after the image is
parsed and before a byte of it runs:

* the effective and the saved uid of the caller become the **owner of the
  file**, and the real uid stays the person;
* a file that the caller may **write** is not a door: a setuid program that
  its caller can replace lets the caller write the code that runs as the
  owner, which is the classic hole of a setuid script. The refusal is
  `EPERM` and the kernel prints the name of the file. Root is exempt, as it is
  for every triad: it may already write anything, and a root shell that needed
  the bit would gain nothing.

**4. The identity is the program's, not the session's.** A task of the kernel
that starts a program - the shell of the console - waits for it, and gets its
own effective and saved uid back when the program ends. The one answer that is
not a return is `K_EXEC_RUN_REPLACED`, where the port has made the image the
memory and the entry of the very task that called the call (a program that
becomes another program, which is the POSIX shape): that task *is* the image
from then on and ends with it, so there is nothing to give back and the
identity is kept to the exit.

This kernel has no per-process credentials: a program that
**forks** during a setuid run hands the identity to its child, and the child
keeps it after the parent returns. That is a hole of a machine whose programs
are tasks of one kernel and whose `fork` runs the parent again in the same
address space (`README.md`, "Known limitations"); the programs that carry the
bit in this release are none, and a setuid program of a machine like this one
has to be written as a program that does not fork.

**5. Only root installs it, and only a signed package may carry it.** `pkg`
reads the flags of the image it is about to install: an image with
`K_EXEC_FLAG_SETUID` is installed by root and by nobody else (`PKG_EPERM`,
with the sentence that says why), and the program lands in `/bin` as a file of
`root:root` and the mode `0755`. The repository signature is what says which
images may carry the flag at all - a package is signed as one file, so the
builder of a repository is the one that decides - and the second half of the
rule is the act of `execve`: a file of `/bin` that a person can write is
refused for that person whatever the installer did. `chmod` cannot grant the
bit: the mode of an object is nine bits, and the flag lives in the image, so
the only way to a setuid program is to build one and install it as root.

**6. The tests.** The matrix of the access is the host test of ADR 0044; this
lane adds the parser of the flag (a valid image with the bit, a flag of a
generation this kernel does not know), the three uids of a task and the rule
of `setuid` (root names any, a task names its real or saved uid, a task of one
uid cannot climb back), the refusal of a setuid image the caller may write,
and the gate of the machine: `/bin/suidcat` - the image of `cat` with the flag
and the owner root - reads `/etc/shadow` for a guest while `/bin/cat` is
refused, `/bin/badcat` - the same image of the mode `0777` - is refused by the
kernel, the shell keeps its own identity after the run, and the file of root
cannot be written by the person who ran it (`make x86-smoke`).

## What this ADR does not bring

* **no setgid bit** and no saved gid (above);
* **no `seteuid`/`setreuid`** of the ABI: a program of this release drops an
  identity with `setuid(getuid())` and returns to it with `setuid(geteuid())`
  only while the saved uid is still the one it was given, which is what a
  careful setuid program of this system does;
* **no audit of the privilege**: the kernel prints a line when it refuses a
  file and nothing when it grants one, and there is no `sudo` and no log of
  who ran what - the accounts are a file and the sessions are a table of
  `/proc`;
* **no setuid on a directory** and no inheritance of a mode: a name that a
  create makes takes the uid and the gid of its maker (ADR 0044 §5), and the
  set-group-id bit of a directory is a bit of the mode this release does not
  have;
* **no program of the release carries the bit**: the two images of the gate
  are copies of `cat` that the image tool of the test makes, and a person who
  wants a setuid program writes one and installs it as root.

## Consequences

**Good**

* A program can be given the identity it needs for the length of its run and
  no longer: the shell of the console is itself before and after it.
* The format, the ABI and the image of every program of the tree are what they
  were: a flag of a field that already carried flags, no new call, no new
  version.
* A hole of a machine of one credential is closed: a program can read
  `/etc/shadow` without a `su` at a console, which is the first thing the
  session of `telnet` could not do.

**Bad**

* A flag of an image is a promise about the file's owner and its mode that the
  image itself cannot check: a person who copies `/bin/passwd` to a directory
  of their own and makes it writable has a program the kernel refuses, and the
  sentence of that refusal is the only teacher.
* The identity of a child of a setuid program survives the parent (above), and
  the machine has no audit of it.
* One more field in `struct task` and one more decision in the check of a
  credential: two numbers that are equal everywhere but in the run of a
  program that carries the bit.
