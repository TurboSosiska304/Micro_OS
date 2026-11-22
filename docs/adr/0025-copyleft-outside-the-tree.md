# ADR 0025 - Copyleft outside the tree

* Status: accepted
* Date: 2026-11-23
* Supersedes: nothing
* Related: ADR 0005 (the programs), ADR 0006 (the entry of a program), ADR
  0016 (the userland), ADR 0018 (the programs of the board), ADR 0020 (the
  signed image), ADR 0024 (the stream and the installer),
  `docs/licensing.md`, `docs/wiki/pkg.md`

## Context

The tree is MIT ([`LICENSE`](../../LICENSE)), and
[`docs/licensing.md`](../licensing.md) is the list of the components of other
people and the rule that keeps the boundary clear. Rule 1 lets permissive code
in; rule 2 keeps copyleft out of a binary of the tree and says that such code
"lives in a repository of its own, or runs as a program of the system (a
user-space driver)" - and ends with "This is the boundary ADR 0025 records".
This is that record.

Until now the boundary was untested: `third_party/` holds three permissive
parts (Spleen, zlib and mbedTLS, the Apache-2.0 half of its dual license), and
a program of the system was a file the tree built and tested itself. `pkg` and
`microos-progs` change the question. A repository of programs is a
distribution channel to a machine whose kernel is MIT (ADR 0024), and the
first GPL program that arrives through it is a licensing decision whether or
not anybody writes one down.

Two properties of the machines fix the shape of the answer. There is **no
dynamic linker**: a program is a flat `MOX1` image, statically linked at a
fixed address, and the libc of the system is compiled into it (ADR 0005, ADR
0016). And the board has **no MMU** (ADR 0018), so even the isolation of
x86-64 ring 3 does not exist there. The kernel and a program are separate
images and nothing is shared at link time; but a library cannot be loaded
beside a program either, so an LGPL library is either absent or linked into
the program. There is no middle ground of a shared object, and the boundary
has to be drawn in the build and in the distribution, not in the page tables.

ADR 0024 makes the repository a tree of its own and keeps the kernel tree out
of the business of building applications. The board has no installer: its
`progs` partition belongs to the firmware, and the port is a compatibility
port from 1.2 on (ADR 0018 §6, ADR 0024 §5). The licensing question therefore
has two faces - a program that arrives through `pkg` on x86-64, and a program
that is shipped in the firmware of the board - and this ADR draws the line
for both, while the second stays a decision of the port.

This is an engineering policy, not legal advice, exactly as the page it
extends says. The rule below is deliberately the conservative side of the
question, so that the project does not have to argue about aggregation or
derivative works on a machine it cannot inspect.

## Decision

### 1. The MIT tree takes permissive code only

MIT, BSD-2-Clause, BSD-3-Clause, ISC, Apache-2.0 (with its patent grant) and
the zlib license may live in `components/`, `ports/`, `main/`, `tools/` and
`third_party/`, under the conditions of `docs/licensing.md` rule 1.

GPL, LGPL, AGPL and CDDL are not linked into **any binary this tree builds**:
not the x86-64 loader or kernel, not a host tool or test, not the MicroFS image
of x86-64, not the firmware of the board. `third_party/` is not an exception
and neither is a file that is compiled and then thrown away; "linked into" is
read mechanically, as one link line that names the file and one image that
contains the result.

A copyleft program is not a part of this tree at all. It is a program of the
system: a file of `MOX1` built and installed by the person who runs it, from
`microos-progs`. The repository of programs may hold it; the kernel tree may
not, and a change that puts such code into a build of the tree is a change
against this ADR even when every test passes.

A dual-licensed part is taken under the permissive half by a written choice,
the way mbedTLS is taken as Apache-2.0 and not as GPL-2.0-or-later. The choice
is a line in `docs/licensing.md`; the other half is not in the tree and is not
a license the project relies on.

Rule 3 of `docs/licensing.md` - no code from the Linux kernel - is not
restated here. Its consequence is a part of the same boundary: a piece of a
GPL driver is copyleft source, so it is not a component of `components/` and
not a row in the table.

### 2. A program is a separate work, and the MMU has nothing to do with it

The kernel and a program of `MOX1` are two programs. The kernel does not link
the program; it reads the header of the file, maps or copies it into a window
and enters it (ADR 0005, ADR 0006, ADR 0018 §3). The program calls the kernel
through the syscall door, `int $0x80`, which is an interface and not a link
(ADR 0016). Nothing of the program's text or data is in the image of the
kernel, and nothing of the kernel is linked into the program.

That is the whole reason a GPL program can run on an MIT system: the two are
distributed and built as separate files, and the user - not the project -
chooses to put the second one on the machine. On x86-64 the program runs in
ring 3 with its own address space (ADR 0005); on the board it runs in a window
of IRAM with the kernel in the same memory (ADR 0018 §4). The difference is
protection and isolation, and both machines still run a separate image. An MMU
would make a program harder to break out of; it would not change which bytes
are linked together, and the license question is about the build and the
distribution.

This ADR does not claim that every use of a copyleft program beside an MIT
kernel is lawful in every jurisdiction. It fixes the engineering rule that
keeps the question from arising inside the tree: no copyleft code is ever on a
link line of this tree, and the tree distributes no copyleft program as a part
of itself. A person who distributes a binary of the system with a GPL program
added is distributing two works and should read the license of the second.

### 3. What a copyleft package carries

A package of `microos-progs` that is copyleft carries, in the repository:

```text
meta/NAME-VERSION-MACHINE.mox.json  license, source and source_sha256
licenses/NAME/                      every license text the package carries
sources/NAME-VERSION.tar.gz         the exact bytes of source_sha256
sources/NAME-VERSION/               the upstream tree as taken
sources/NAME-VERSION.patch          every change the port takes, in order
sources/NAME-VERSION/BUILD          the command that produced the package
```

The requirements, in the order a reader checks them:

1. **`license`** names the license the package is distributed under, as SPDX
   identifiers (`GPL-3.0-or-later`, `LGPL-2.1-only`, or a list when the
   program carries more than one part). It is the field the installer reads;
   a package without one is refused (§4).
2. **The texts** under `licenses/NAME/` are every license a package carries:
   its own and those of the libraries statically linked into it. A binary
   carries the licenses of what it contains (rule 4), and the texts are not
   merged into the MIT `LICENSE` of the tree.
3. **`source` and `source_sha256`** name the exact source, not a page that may
   move. `source` may be a URL for a reader; `source_sha256` is the digest of
   the tarball in `sources/`, and that tarball is what the package was built
   from.
4. **The patches are the port.** A change to an upstream file is in the patch
   file with a line saying why; the tarball is upstream plus that patch, so
   the package can be rebuilt years later, on another machine.
5. **`BUILD`** is the command that produced the package, with the toolchain
   and the flags. It is the record a person reads to rebuild the program, and
   the input of the SDK build of §6.
6. **An LGPL program stays relinkable.** There is no dynamic linker on either
   machine, so the LGPL cannot be satisfied by a shared library. A package
   that statically links an LGPL library ships the object files it was linked
   from and the link command under `sources/NAME-VERSION/`: with those the
   user can relink the program against a modified library, which is what LGPL
   §6 asks for in a system without one.

A permissive package carries at least `BUILD` and `source`; the tarball is
preferred, because a URL can disappear. The ceiling of `PKG_PACKAGE_MAX` (1
MiB) applies to the program the installer allocates, not to the tarball or the
license texts: no client of 1.2 downloads them, and a source tree is allowed
to be larger than the image it produces.

### 4. The installer refuses a copyleft package without its source

The index carries three fields that matter here - `license`, `source` and
`source_sha256` - and `pkg` reads them from 1.3 on. The parser of the index
already skips a field it does not know, so a 1.2 machine keeps installing from
a newer repository without a change (`components/pkg/include/pkg/pkg.h`); the
agreement is additive and no field changes its meaning.

The contract of `pkg` from 1.3 on:

* **a package without `license` is refused.** The license is a condition of
  the distribution, not an optional note;
* **a copyleft package without both `source` and `source_sha256` is
  refused.** The test is textual and runs on the name in the field: a
  `license` that begins with `GPL-`, `LGPL-` or `AGPL-` is copyleft for the
  installer. CDDL is copyleft at this boundary as well, and a CDDL package is
  held to the same source rule by policy, though the mechanical check of 1.3
  names the three GNU families;
* **the answer is its own code.** A package that is refused for its license is
  not a malformed index and not a bad signature: the JSON was well formed and
  the bytes were authentic. The installer answers `PKG_ELICENSE` (-13), the
  next free number after `PKG_EEXIST` (-12), so the sentence a person reads
  names the license and not a parser, and no existing number moves;
* **the refusal is not a switch.** `--allow-downgrade`, `--needed`,
  `--noconfirm` and the rest of the console flags of ADR 0024 §6 do not touch
  it. There is no `--force` that installs a copyleft package without its
  source; the cure is the source, not a flag.

A dual-licensed package names the half it is distributed under in `license`;
the check reads that name, so a package taken as MIT does not trigger it and
one taken as `GPL-2.0-only` does.

Two gates enforce the rule, and they know different things. `tools/mkindex.py`
of `microos-progs` writes the fields and `make check` verifies the digest of
the tarball and the presence of the texts at publish time. `pkg` verifies the
presence and the shape of the fields at install time - it does not download
the source, so it cannot verify the digest of a tarball it never sees. The
first gate is the one that proves the source is the source; the second is the
one that refuses a package that arrived through a mirror with a broken or
hostile index.

The rule of this section is about a package of an index. `pkg install ./x.mox`
(`-U`) reads a file the person already has, not a package of a repository:
there is no `license` field to read, and the act is the person's own, not a
distribution by the project. The file still has to carry a signature, as ADR
0024 §4 says.

### 5. The signature of the index, and what it does not sign

The index and every program are signed with Ed25519 and the same key as the
images of the system (ADR 0020 §4, ADR 0024 §4). The default repository is
`microos-progs` (`PKG_DEFAULT_REPO`), and its release machine signs; the
private key never enters either tree and lives outside them, at
`~/.config/microos/keys/microos-progs.key` on that machine. The public half is
`keys/microos-progs.pub` in the repository and a copy at its root for `wget`;
a machine reads `/etc/pkg/keys/<repo>.pub` and falls back to the key of the
image it booted (`pkg_repository_key`; `docs/wiki/pkg.md`).

A key is rotated by publishing the new public key in a commit of its own,
before the first index signed with the new private key, and by announcing the
old and the new key in the release notes of the kernel. An index signed by a
key the machine does not know is refused, so the rotation is a trust decision
of the person, not a silent update: a machine that has not received the new
key stops installing until it does. The key of the image is a separate
rotation and still means a new loader (ADR 0020 §4); the key of a repository
is a file.

The signature answers "these bytes came from the holder of this key", and
nothing else. It does not answer "this program may be distributed": that is
the license, and no signature makes a package lawful. A repository that signs
a GPL package without its source has produced a well-signed violation, and
`pkg` refuses it on the license field before the signature has anything to
say. The two checks are independent, both run, and the stricter one wins. The
signature does make the distributor accountable: the holder of the private key
is the person whose name is on the package.

### 6. What stays outside: modules, drivers, the build

The kernel does not load code into itself. There is no `.ko`, no module ABI
and no loader of one: a program is a file that `execve` starts, and a driver of
the Linux kind leans on an interface this kernel does not have (ADR 0018 §6,
`docs/licensing.md`, "What is not in the tree, and why"). A copyleft driver is
therefore never a file of `components/`: it is a program of its own, in a
repository of its own, that talks to `/dev` - a user-space driver in the words
of rule 2 - and the user installs it like any other program, with the rules of
§3 and §4. A driver that cannot be written that way is not written for this
system.

The build of a copyleft package happens on the release machine, with the cross
compiler of the SDK (`make sdk`, 1.3), against the headers and the linker
script of the tree. It does not happen on the device: the machine has no
compiler, and an install script is code of a stranger run as root (ADR 0024
§6). `BUILD` and the patches are what let a person rebuild the package
somewhere else; the device only installs the result.

The board is the second face of the boundary. It has no installer, and its
`progs` partition belongs to the firmware (ADR 0018 §6, ADR 0024 §5). A
copyleft program for the board would be a separate `MOX1` image shipped in the
firmware bundle - never linked into the firmware itself - and the same rules
would apply to the bundle: the texts, the source and the patches travel with
it. That is a decision of the port and is not taken by this ADR; what is taken
is that such a program is never a part of the firmware image.

## Consequences

**Good**

* The tree stays MIT in its code and permissive in its binaries; a person can
  tell what is in the kernel, the loader and the images from the table of
  `docs/licensing.md`.
* Copyleft software is possible on a system whose kernel is MIT, because a
  program is a file and the user installs it.
* The boundary is mechanical: one list of permissive licenses, one list of
  copyleft families, three fields in the index and one answer of the
  installer.
* A copyleft package can be rebuilt from the repository years later, on
  another machine, with the exact source and the patches the port took.

**Bad**

* A copyleft package costs more than a permissive one: the tarball, the
  patches, `BUILD`, the texts and, for LGPL, the object files.
* The enforcement arrives with 1.3: the `pkg` of 1.2 skips the fields, so a
  1.2 machine installs a package that a 1.3 machine refuses.
* A static LGPL link is relinkable only in theory on the device: the user
  needs a build machine and the SDK. The rule is met as far as this system
  can meet it, and the debt is the SDK, not the license.
* The boundary is conservative. A lawful arrangement that a lawyer might
  accept - a GPL library linked into a program of the tree, for example - is
  not available here, by choice.

**What the ADR does not do**

* It does not change `LICENSE`, relicense a file or add a component to the
  table of `docs/licensing.md`.
* It does not decide whether a particular program is a derivative work of the
  kernel; that is a legal question, and the rule makes sure the project does
  not have to answer it for its own binaries.
* It does not add the fields to `pkg.h` or the parser. It fixes the contract
  they implement in 1.3, including the answer `PKG_ELICENSE`.
* It does not give the tree a module loader, a compiler on the device or an
  AUR-style build of a package during an install.
* It does not make the board an installer, and it does not decide which
  copyleft program the board may carry in its firmware bundle.
* It does not govern the programs a person writes for themselves; their
  license is their choice, and `pkg -U` installs a file they signed.
