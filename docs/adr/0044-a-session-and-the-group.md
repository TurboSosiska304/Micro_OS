# ADR 0044 - A session of the accounts, and the group a record does not keep yet

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0005 (programs and protection), ADR 0006 (the entry of a
  program), ADR 0016 (the userland), ADR 0022 (the layout of 1.1), ADR 0029
  (ext2 write), ADR 0038 (a table of records), ADR 0033 (the accounts, first
  half), `docs/plan-1.6.md` M5, `docs/plan-1.8.md` M6, `docs/debts.md`,
  `docs/syscall-abi.md`

## Context

ADR 0033 made a person a file of two files: `/etc/passwd` names the account
and `/etc/shadow` keeps the hash, `login` and `su` are commands of the shell of
the kernel that move the uid and the gid of the running task, and every object
of a filesystem has an owner and a triad. It also wrote down what it did not
build, and the cleanroom of 1.6 found one of those refusals to be a defect:
`login` read its password with `k_console_getchar()`, which is the UART of the
machine, so a person at the other end of a telnet connection typed a password
into a keyboard somewhere else - or could not log in at all.

Three things of the same half of the accounts were still open when 1.8 began:

* a **session**: there was no `logout`, no `who`, no record of who is in, and
  a shell that became a user could only become root again by a reboot;
* the **owner of a new file**: the create path of the VFS carried no uid, so a
  file made by uid 7 was owned by root (the debt of 1.5 and 1.6);
* the **group of a record**: the gid existed in the task and in the line of
  `/etc/passwd` and decided no permission, because no filesystem record held
  one and the check of the access read only the owner and the others.

This ADR records what M6 of 1.8 built for the first two, and - the larger half
of it - why the third is still not built: it is a decision of the format and
of the ABI, and the shape of that decision is written here so that the release
that takes it does not have to rediscover it.

## The question

What is a session on a machine whose console is the shell of the kernel, who
is allowed to open one and where does its password come from, and what does a
group mean when a record of a filesystem has none.

## The decision: a session is a task with a terminal

**1. A task has a terminal of its own** (`kernel/console.h`,
`struct k_terminal`): the bytes it reads and the bytes it writes, which are
the console of the port when it has none. `k_console_getchar()` and
`k_console_write()` follow the terminal of the calling task, so a dialogue -
the name, the password, the lines of the shell, the question of `su` - reads
and writes the terminal it was started on and nowhere else. That is the whole
of the defect of 1.6: the password of a session of telnet comes from the
socket and a password typed at the board comes from the UART.

**2. The output of a session is its terminal.** A task with a terminal writes
to it and not to the port and its sinks, so two people of the machine do not
read each other's answers; the UART, the panel and their sinks stay the
console of the tasks that have no terminal. The echo is negotiated: the server
of telnet offers `WILL ECHO` (RFC 857) and repeats the bytes when the client
accepts, and a secret line is never repeated by either half.

**3. The init of every port is the getty.** A session is a task
(`config/session.h`), and the init of the x86-64 machine, of the board and of
the simulator makes one, waits for it, and makes another one when a `logout`
ended it. A session cannot climb back to root by itself: this kernel has one
credential per task and no saved uid (ADR 0005), so the task that makes the
next session is the root init and not the session that left.

**4. The console is root without a password.** The person in front of the
machine *is* the machine: the console session starts as root and asks nothing,
`login` asks for a password to become somebody else, and `logout` returns to
the root prompt. This is the rule of ADR 0033 made a rule of the session: the
password is the door of `su` and of the wire, and it is not a lock on the
physical console. A machine that needs the console locked is a machine with a
getty of its own and a policy in `/etc`, which this release does not have.

**5. A session of telnet is the same session behind a password.** The
listener stays root and makes a session task on each connection: the name and
the password (three attempts, a wait of 1.5 s and one sentence for a wrong
password and for a name that does not exist - a hint about which one was wrong
is a list of the accounts), the home of the account, the shell as that user,
`logout` and the close. The hash is the `$sha256$` of `/etc/shadow` and
nothing new is added; a name that does not exist costs the same hash as one
that does, so the wait does not tell them apart either.

**6. `who` is made of the table of the tasks.** A task says what kind of
session it is (`TASK_TTY_NONE`, `TASK_TTY_CONSOLE`, `TASK_TTY_TELNET`) and
`/proc/sessions` prints the ones that are in: a utmp that a reset cannot leave
stale and that nobody has to delete a line from. `who` reads it and names the
uid through `/etc/passwd`; the kernel keeps no table of names.

**7. The owner of a new object is its maker.** `vfs_open()` with `O_CREAT` and
`vfs_mkdir()` give the object they made to the uid of the task that made it,
through the `chown` operation of the filesystem. The record is born with the
owner of root because a filesystem has no task; the VFS is the layer that
knows who is calling. An open of a name that was already there moves nothing.

## The decision: the group waits

**There is no group in the record still, and no `/etc/group`.** ADR 0033
refused both, and 1.8 refuses them for a reason of its own: half of the
machinery is in place and the other half is not, and a group that decides
nothing is worse than a group that is not promised.

What exists today:

* a MicroFS record of version 3 has a `group` word (ADR 0038) and
  `mfs_inode_group()` and `mfs_chgrp()` read and write it;
* `statx` answers it in the hole of the alignment of `type`, so `struct
  statx` of the ABI did not grow (it is 40 bytes) and a program of 1.7 reads
  the field it knows;
* `struct vfs_stat` - the question the check of the access asks - has **no**
  group, and no filesystem fills one for ext2, FAT32, procfs or devfs.

What is missing, and what the release that takes the group has to do:

1. **a way to set it.** `vfs_operations` grows `int (*chgrp)(void *ctx, const
   char *path, k_u32_t group)` and an adapter in each filesystem that has a
   group: `mfs_chgrp()` for MicroFS, `i_gid` for ext2, nothing for FAT32
   (answer `KERR_NOTSUP`). The kernel of 1.8 cannot fill the table for MicroFS
   without touching `components/mfs`, and a call that no filesystem of the
   image implements is a promise the machine cannot keep;
2. **a call of the ABI.** The next free number of `kernel/ksys.h` is 48:
   `chgrp(path, gid)` beside `chown`, root alone, or the owner of the object
   for its own group if the rule is widened. The numbers are frozen, so the
   call takes 48 and the documentation of `docs/syscall-abi.md` is written
   with it;
3. **the check of the access.** `struct vfs_stat` grows `k_u16_t group` (a
   kernel structure, not the frozen ABI), every filesystem fills it, and
   `caller_bits()` in `components/kernel/src/vfs.c` reads the triad of the
   group when the task is not the owner and its gid is the group of the
   object. Root keeps its bypass of the three triads and of the execute bit,
   as POSIX has it;
4. **`/etc/group`** and `getgrgid()`/`getgrnam()`: `name:gid:member,member`,
   read by the same rule as `/etc/passwd` (a line that does not fit stops the
   read), written by `users_defaults()` and by the image tools, with the
   primary group of a uid from the gid field of its line in `/etc/passwd`.
   There is no `newgrp` and no supplementary group in a task: one gid per
   task is what the ABI has, and a second list is a change of `struct task`
   and of every entrance;
5. **the group of a new object.** `vfs_open()`/`vfs_mkdir()` set the group of
   the object from the gid of the maker with the same act that sets the
   owner, and a directory with the set-group-id bit gives its group to its
   children - which is a bit of the mode and a decision of the format, and the
   day it comes it comes with the bit of the setuid of `MOX1` (ADR 0033, K8 of
   `docs/debts.md`);
6. **the tests**: the triad of the group on a record of each filesystem, a
   task of gid 7 that reads a file of mode 0640 and group 7 and does not write
   it, a task of gid 8 that does neither, a `chgrp` that root may do and a
   task of the object may not, and `/etc/group` broken in three ways.

**The setuid bit of `MOX1` stays out**, with the shape ADR 0033 wrote: a flag
in the header of the format (a new version and a migration for every image of
the tree), a saved uid in `struct task` (real, effective and saved, which is
what a careful setuid program needs and what `seteuid` would move), a mode of
the image that `pkg` signs and a rule for the installer, and nine bits of
`chmod` that do not hold it. It is a lane of its own, not a line of M6.

## What this ADR does not bring

* **no `passwd`**: a person changes a password by editing `/etc/shadow`;
* **no key-derivation function**: the hash is the `$sha256$` of ADR 0033 and
  a fast hash of a short password is what an attacker with the file has;
* **no `/etc/group`, no `newgrp`, no supplementary groups** (above);
* **no session of a program**: the terminal is the one of a task of the
  kernel, so a program started from a session of telnet writes its `stdout`
  to the console of the UART (its descriptor 1 is not the terminal) and reads
  its `stdin` from the console too. A session of a program needs the
  descriptors 0, 1 and 2 of the entry to be the terminal of the task, which is
  the same work as a socket call of the ABI (`docs/debts.md`);
* **no login of the serial console after a boot**: the console starts as root
  and a machine that needs a door on the UART is a getty with a policy, not a
  session of telnet with a cable.

## Consequences

**Good**

* The password of a session comes from the terminal that person is on; the
  defect of 1.6 is a test of the host and thirteen checks of a socket in
  QEMU, including the proof that COM1 never sees `password: `.
* A machine of more than one person: `logout` returns to the door, `who` says
  who is in, and the file a person makes is theirs.
* The record format, the ABI and the image are untouched except for the owner
  of a new object, which the filesystems always had a field for.

**Bad**

* A session costs a task: 8 KiB of the internal RAM of the board per client of
  telnet, and the listener of telnet is one more task.
* Two sessions share one kernel and one filesystem, and a program started from
  a session of telnet still reads and writes the UART.
* The group is still half a feature: a task has one, a MicroFS record can keep
  one, and the check of the access ignores it.

## What this ADR does not decide

* It does not add the group to a record, and it does not forbid it: the shape
  above is the work of 1.9.
* It does not add the setuid bit of `MOX1`.
* It does not change the syscall ABI of 1.8: `getuid`, `geteuid`, `getgid`,
  `setuid`, `setgid` keep their numbers and their rules.
* It does not give the prompt the name of the user: `id`, `whoami` and the
  line of `login` say who the shell is, and every script of the tree reads one
  prompt.
