# ADR 0033 - The second half of the users: a file of accounts, a console, and no setuid bit

* Status: accepted; the session and the door of the network are extended by
  ADR 0044 (1.8), and the refusal of the setuid bit and of the group of a
  record still stands
* Date: 2026-11-23
* Related: ADR 0005 (programs and protection), ADR 0006 (the entry of a
  program), ADR 0016 (the userland), ADR 0020 (the signed image), ADR 0022
  (the layout of 1.1), ADR 0029 (ext2 write), `docs/plan-1.6.md` M5,
  `docs/syscall-abi.md`, `docs/debts.md`

## Context

1.5.0 made a task a user. Every `struct task` carries a uid and a gid, a fork
and a program inherit them (`task_creds_inherit`), the create of a task takes
them from the creator, `setuid` and `setgid` of the ABI move them with one
rule (root may name any uid of the fifteen bits a record holds, a task of
another uid may only name itself), and the uid decides the triad of every
object: root passes, the owner reads its own triad, everybody else the triad
of the others. `chown`, `mount`, `umount` and a signal to a task of another
user belong to root.

What 1.5.0 did not have is the other half: **a name**. The uid was a number a
test could move and a person could not. There was no file of accounts, no
`login`, no `su`, and no way for a program to run as somebody else: the boot
and the shell were root, and `docs/debts.md` recorded the gap with the shape
of the work ("a file of accounts, `login`/`su`, a policy of the shell that
starts a program as the uid of the person, and a way back to root").

This ADR records what was built for M5 of 1.6, and — the larger half of it —
what was deliberately **not** built: a setuid bit in the image of a program
and a gid in the record of a filesystem. Both are format changes. Both are the
obvious next step of a Unix and both are refused here, so the refusal needs
an argument and not a silence.

## The question

How does a person become a user of this machine, which file says who the users
are, how is a password checked, and does a program of the disk get a way to
run as another user.

## The decision

**The console is the door, and the files of `/etc` are the accounts.**

1. `/etc/passwd` holds `name:uid:gid:home:shell` and `/etc/shadow` holds
   `name:hash`. They are the truth: a lookup opens the file and reads it, the
   kernel keeps no table of names, and the library of a program
   (`getpwnam`, `getpwuid` in `components/libc/user/src/pwd.c`) reads the same
   bytes as the shell. A person who edits `/etc/passwd` with `edit` has the
   new account at the next `login`.
2. The hash is **salted SHA-256**: `$sha256$<salt>$<64 hex digits>` over the
   bytes of the salt followed by the bytes of the password. The digest is the
   one of the loader (`ports/port_x86_64/loader/sha256.c`), the code that
   verifies a signed image, compiled into the configuration a second time: one
   hash in the tree, not two that agree by accident.
3. `login` and `su` are **commands of the shell of the kernel**, not programs
   of `/bin`. The console of this machine *is* that shell and it starts as
   root; there is no getty and no `init` that reads a line before it. So
   `login` asks for the name and the password and hands the *running* shell to
   the user: `users_apply` enters the home directory as root and then moves
   the two credentials of the task in one critical section
   (`task_set_creds`), the gid before the uid. Every program started from then
   on inherits the identity.
4. **There is no setuid bit in `MOX1`.** A program cannot change the identity
   it runs with; it inherits the uid of the shell that started it.
5. **There is no gid in the record of a filesystem.** The gid exists in the
   task and in the line of `/etc/passwd`; it decides no permission, and the
   triad of the group of a mode is never the one the check reads.

## The format of the two files

`/etc/passwd`, one account per line, five fields, `#` for a comment:

```
root:0:0:/:/bin/sh
guest:7:7:/home/guest:/bin/sh
```

* the name is 1 to 15 characters, without `/`, `:` or a space;
* the uid and the gid are decimal and at most 32767, the fifteen bits of a
  record of a filesystem (`KSYS_UID_MAX`);
* the home and the shell are up to 47 characters, and either may be empty;
* a line with four fields, with six, with a number that is not a number, or
  with a field that does not fit is **not** a record, and the read stops
  there: a file of accounts is read whole or not at all, because half of it is
  a different set of users;
* the first line that matches wins;
* there is no password field and no GECOS field. The password lives in
  `/etc/shadow` and nothing of this machine shows a GECOS.

`/etc/shadow`, one line per account, `name:hash`:

```
root:$sha256$mox1$da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c
guest:*
```

* `$sha256$<salt>$<64 lowercase hex digits>` with a salt of 1 to 16 characters
  and no `$` in it;
* an empty hash, `*` and a hash that begins with `!` are a **locked** account:
  no password opens it, and an account with no line at all is locked too;
* a hash of another form (`$md5$...`, a plain word) is refused, not guessed
  at: a file that looks like a shadow file and is not one must fail closed;
* the comparison of the digest does not stop at the first digit that differs;
* the file is `0600`, and the passwd file `0644`. A program of uid 7 that
  opens `/etc/shadow` is refused by the mode, which is the one thing the
  shadow file buys over the passwd file.

The default of the tree is `root` with the password `microos` and `guest`
(uid 7, gid 7, home `/home/guest`) with **no** password. It is written by the
image tool of x86-64 (`tools/x86_image.py`, with Python's `hashlib`), by the
factory defaults of the board and by the simulator (`users_defaults`), and the
smoke test proves that the C side reads what Python wrote. A machine whose
console is physical and whose shell starts as root is not protected by a
password: the password is the door of `su` and of the `login` a person types,
and the default is a convenience with a sentence in the release notes, not a
secret.

## What `login` and `su` do

`login [name]` — only root may open a session (a task that gave up root cannot
take it back, so a second `login` from a user would be a lie). It asks for the
name when it is not given and for the password **three times**; a wrong
password after the third answer closes the door and changes nothing. On
success it enters the home of the account and moves the credentials, then says
who the shell is:

```
microos> login root
password:
login: root, uid 0, gid 0, home /
```

`su [-|-l] [name]` — the name is `root` by default. **Root becomes anybody
without a password**; a user is asked for the password of the account it
becomes, once, and a wrong one is `su: authentication failure` with nothing
changed. `-` (or `-l`) makes the home directory the directory of the shell, as
a login does:

```
microos> su - guest
microos> pwd
/home/guest
```

`id` prints what the shell is:

```
uid=7(guest) gid=7 home=/home/guest shell=/bin/sh
```

A task of another uid that `su`s to itself needs no password (it already is
that person). The way back to root from a user is a reboot or the password of
root — which `su root` accepts, because `su` is the privileged act of the
console and not a program of the disk. The prompt does not change: `id` and
the line of `login` say who the shell is, and every script of the tree reads
one prompt.

## Why no setuid bit in `MOX1`

The bit is cheap to write and expensive to own.

* **It is a format change.** `MOX1` is a frozen contract (ADR 0006, ADR 0011,
  ADR 0022): a flag in the header is a new version of the format, a migration
  for the images of the tree, and a rule for every port that writes one. That
  is a release of its own, not a line of a lane.
* **The kernel has one credential and no saved uid.** There is no saved
  set-user-ID and no `seteuid`: a program that enters as root stays root for
  its whole life, cannot drop the privilege and take it back, and cannot be
  written the way a careful setuid program is written. A setuid bit over this
  ABI is a setuid *shell script*, the classic mistake, made a property of the
  system.
* **The machine has no way to audit it.** There is no `sudo`, no log of a
  privilege, no per-file mode of an image beyond the nine bits of `chmod`, and
  no package signature that says which images may carry the bit (`pkg` signs a
  package, not a bit of one of its files).
* **Nothing needs it yet.** The programs of the tree that would want it
  (`ping`, `mount`, a mail queue) do not exist here, and the console already
  gives the person every identity through `su`. The day a program needs it,
  the bit comes with a saved uid, a mode of the image and an ADR of its own.

The same argument, one step weaker, applies to a **gid in the record**:
MicroFS has no room for it without a new version (ADR 0022), FAT32 has spare
bits and ext2 has the field natively (ADR 0029), and the check of a mode would
have to grow the group triad in `caller_bits` and `st_gid` in `struct
vfs_stat`. The gain is a group that decides a permission no program of this
tree asks for. The gid stays a number a task carries and a line of
`/etc/passwd` names.

## What this ADR does not bring

The list is the debt of the lane, and it is in `docs/debts.md` with the rest:

* **no `passwd`**: a person changes a password by editing `/etc/shadow`, and
  the recipe is `printf 'salt%s' password | sha256sum`. The kernel port has no
  source of entropy (`k_port_random` does not exist; the TLS client has one of
  its own), so a salt cannot be drawn by the machine yet;
* **not bcrypt and not argon2**: a fast hash of a short password is a hash a
  dictionary attacks at full speed once the file is read. The salt defeats a
  table of digests and nothing else;
* **no `/etc/group`**: `getgrgid` answers the group of root and `getgrnam`
  its name, and nothing else;
* **no sessions, no `utmp`, no `last`, no `logout`, no `who`**: the console is
  one shell and the identity of a task is the session;
* **no `chdir` for a program**: the directory of a program is a table of the
  library (`components/libc/user/src/path.c`), not a call, so `su -` moves the
  directory of the *shell* and not of a program that a person starts later
  from another directory;
* **the file a user creates is owned by root**: `vfs_open` carries no owner
  into the create path, so a file made by uid 7 has owner 0 until root
  `chown`s it. This is the debt of 1.5 (docs/debts.md) and the smoke test
  asserts the debt instead of the rule;
* **the files of `/etc` longer than 127 characters do not survive a format**:
  the key-value store of the port gives a value 128 bytes, so `/etc/shadow` of
  one account is saved and a file of several is not (`config persist` says so
  in a warning). The defaults come back at the next boot; a person's longer
  file does not.

## Consequences

**Good**

* A name is a file of the disk, so a person can add a user with `edit`, a test
  can write one, an image of the machine carries the same two files, and the
  library of a program and the shell cannot disagree.
* The console has a door: `login`, `su`, three attempts, a locked account, a
  home directory, and a refusal that says why. A program started from a
  session runs as that person and inherits the uid through fork and execve
  without a line of code in the program.
* The image format, the record of every filesystem and the ABI of the calls
  are untouched. Nothing of 1.5 has to be migrated.
* The hash is the SHA-256 the tree already had, in one implementation, and the
  format has a place for a better one without a new field.

**Bad**

* The machine is not a Unix: a program cannot be setuid, so a door that a
  program opens (a network service that drops to a user, a `passwd` that
  writes a shadow) has to be a command of the kernel's shell or a call of the
  kernel. That is a real limit and the reason the next program that needs it
  will reopen this ADR.
* A default password that cannot be changed by the machine is a password that
  stays on every image until a person edits a file with a recipe.
* The gid is half a feature: a task has one, a file has none, and the mode
  check ignores the group triad.
* `login` is a command of the shell and not a process, so a shell that became
  a user cannot become root again; a person who wants root back reboots.

## What this ADR does not decide

* It does not add the setuid bit, and it does not forbid it forever: the bit
  is a change of `MOX1` and needs its own ADR, a saved uid in `struct task`
  and a rule for `pkg`.
* It does not add a gid to a record, and it does not add `/etc/group`.
* It does not choose a key-derivation function: `$sha256$` is the name of the
  hash this version writes, and a `$argon2id$` prefix is the way a later
  version would write another one beside it.
* It does not change the syscall ABI: `setuid`, `setgid`, `getuid`,
  `geteuid` and `getgid` keep their numbers and their rules.
* It does not give the console a getty, and it does not make the prompt say
  who the user is.
