# ADR 0030 - The license of the tree: MIT, or MPL-2.0

* Status: proposed
* Date: 2026-11-23
* Related: ADR 0011 (versioning and 1.0), ADR 0005 (programs and protection),
  ADR 0016 (the userland), ADR 0020 (the signed image), ADR 0024 (the stream
  and the installer), ADR 0025 (copyleft outside the tree),
  `docs/licensing.md`, `docs/porting-gpl.md`, `docs/plan-1.4.md`,
  `microos-progs`

## Context

The tree is MIT ([`LICENSE`](../../LICENSE)), and
[`docs/licensing.md`](../licensing.md) is built on that sentence: rule 1 lets
permissive code in and says the tree "takes permissive code only"; rule 2 keeps
copyleft out of every binary the tree builds; ADR 0025 records the boundary
that rule draws - a GPL program is not a file of this tree, it is a package of
`microos-progs` that the person installs. The first such package, `patch` 2.5.4,
is `GPL-2.0-or-later AND MIT`, because the library of a program,
`libmicroos_user.a` from `components/libc/user/`, is compiled into the image
with the GPL code: there is no dynamic linker (ADR 0016), so the two licenses
meet inside one binary.

MIT has worked because it asks almost nothing: keep the notice, and the rest is
the person's choice. The price is that the tree has no answer to a vendor who
improves `components/kernel/src/sched.c`, ships a product and publishes
nothing. From 1.4 the question sharpens: the SDK ships a sysroot and a static
library, the x86-64 bundle ships a whole machine, `pkg` installs GPL programs
onto it, the board ships a firmware image. Each is a distribution, and the
license is what a lawyer reads first.

This ADR asks whether the files this project writes should stay MIT or become
MPL-2.0, and in which shape. It is an engineering analysis, not legal advice: it
reads the texts, names the places of this tree they touch, and says where a
person with a legal education has to answer.

## The question

Should the code authored here move to MPL-2.0 - from which version, since 1.0 to
1.3 are out under MIT and cannot be recalled - with which exceptions for the
part compiled into someone else's program (`components/libc/user/`), and at what
price in files, bundles and rules.

## Options

**A. Stay MIT.** Nothing changes except the notice gap of §2.6, which MIT asks
for anyway.

**B. MPL-2.0 for the whole tree.** One text, one rule; every file carries the
notice of Exhibit A; the library of a program is MPL too.

**C. MPL-2.0 for the core, MIT for the library of a program.** `components/`
(except `libc/user`), `ports/`, `main/` and `tools/` become MPL-2.0;
`components/libc/user/` and the headers of the SDK stay MIT, and
`docs/licensing.md` names that boundary as the one exception.

**D. Apache-2.0.** Permissive, with a patent grant and a termination of its own,
and a `NOTICE` file the license requires.

**E. AGPL-3.0 or BSL-1.1.** Named for completeness; §5 says why neither fits.

## Analysis

### 1. What MPL-2.0 gives, and what it does not

MPL-2.0 is a **file-level** copyleft. Its unit is the "Covered Software": the
source files to which the initial contributor attached the notice of Exhibit A
(§1.4), plus their Modifications - a file changed, or a new file that contains
Covered Software (§1.10). What §3 asks is narrow and readable:

* **source** form stays under the MPL, with the notice and the text, and the
  recipient's rights may not be restricted (§3.1);
* **executable** form must also make the source of those files available and
  tell the recipient, "by reasonable means in a timely manner", how to obtain
  it, at no more than the cost of distribution (§3.2(a)); the executable may be
  under other terms, provided they do not limit the recipient's rights in that
  source (§3.2(b));
* a **Larger Work** - our files combined with other material "in a separate
  file or files" (§1.7) - may be distributed under the distributor's terms, as
  long as §3.1 and §3.2 are met for our files (§3.3).

There is a **patent grant**: every contributor licenses the claims necessarily
infringed by their contribution, alone or combined with the covered software
(§2.1(b)), but not those infringed by your own modifications or by the
combination with other software (§2.3). It ends if you sue somebody alleging
that a contributor version infringes a patent (§5.2), and that termination takes
the copyright grant with it; §5.1 cures ordinary non-compliance and there is no
cure for §5.2. §5.3 keeps the end users' sublicenses alive, so a terminated
distributor does not strand the machines in the field.

What matters most for **this** tree is §3.3's second paragraph with §1.12: a
Secondary License is "the GNU General Public License, Version 2.0, the GNU
Lesser General Public License, Version 2.1, the GNU Affero General Public
License, Version 3.0, or any later versions of those licenses". Our files may
therefore be combined with GPL-2.0, GPL-3.0, LGPL-2.1 or AGPL-3.0 code and the
combination distributed under that GNU license, with our files left available
under the MPL as well - the FSF describes exactly this and calls the MPL files
dual licensed. That is the one compatibility trick Apache-2.0 does not have.

What MPL-2.0 does **not** do is as important. It does not touch **new files**: a
vendor who writes a driver, a port or a subsystem in files of their own owes
nothing but the notices of the files taken unmodified - and for a kernel whose
unit of contribution is "a new port in a new directory", that is most of the
work a commercial user would keep. It does not force **contribution**: §3.2
asks that the source be available and the recipient told where, not that a patch
be sent upstream. It protects no **interface**, format or idea and grants no
trademark right (§2.3). It adds no **patent portfolio**: the grant covers only
what a contributor owns.

### 2. What it does to the places of this tree

#### 2.1 The kernel and the ports

`components/` and `ports/` stay readable, forkable and usable in a product - a
vendor may ship a closed device whose firmware contains Micro OS, on one
condition: the files we wrote, if modified, are published, and the recipient is
told where the source of the others is. The repository is public and every
release has a tag, so the duty costs a line in a manual.

The real work is the rule. "The MIT tree takes permissive code only" becomes
false: MIT, BSD, ISC and zlib may still come in (a permissive file keeps its own
license and notice, and §3.3 lets it sit in the larger work), MPL-2.0 may come
in, and GPL/LGPL/AGPL/CDDL still may not - for the reason of ADR 0025, not for
the reason of MIT. The rule becomes "the files of this tree are MPL-2.0; a
vendored file keeps its own license; a copyleft file never enters a binary this
tree builds".

#### 2.2 The library of a program, and the GPL packages

This is where the two licenses meet. Today the library is MIT,
`licenses/patch/MIT-libmicroos_user` is a copy of the tree's `LICENSE` beside
the GPL text, and the package's `license` field reads `GPL-2.0-or-later AND
MIT`. Under MPL-2.0 there are two routes and both are lawful:

* **§3.3 with a Secondary License.** For `patch`, `diff`, `sed`, `gzip` and
  `make` - GPL-2.0-or-later or GPL-3.0-or-later - the image is a Larger Work
  that combines MPL files with a GPL work, so the MPL half may additionally be
  distributed under the GNU license and the recipient may treat the library as
  MPL or as GPL. The field becomes `GPL-2.0-or-later AND MPL-2.0`, which says
  what the image is: two licenses, each over its own files;
* **§3.2 alone.** For a license that is not a Secondary License - the
  GPL-2.0-*only* case, and BusyBox is the classic - the library's source must
  be available and the recipient told where. Nothing forbids the combination,
  and §1.12 names "Version 2.0" explicitly, so this case is covered too; it is
  the one where a lawyer is worth asking rather than this page.

Most of §3.2 is already true by fact: the source of the library is the public
tree at the tag the SDK was cut from, `sources/` carries the tarball and the
`BUILD` of each package, `licenses/NAME/` carries every text. What is missing is
the sentence that turns a fact into compliance - a package naming the library
version and where its source lives - and the `license` field of `hello` (MIT, no
library mentioned) becomes `MIT AND MPL-2.0`. Then `pkg` has a hole:
`PKG_ELICENSE` refuses a `GPL-`, `LGPL-` or `AGPL-` package without `source` and
`source_sha256`, and `MPL-2.0` is copyleft at this boundary but not on the list.

Option C exists because of this. With the library left MIT none of it is needed:
the package keeps `AND MIT`, no new field, no new family in `pkg`, no argument
about §3.3, and a person who writes a closed program links the library exactly
as today and owes nothing. The kernel is MPL, and the boundary is the syscall
ABI, which is already the boundary in ADR 0016.

#### 2.3 Drivers, SDL, and the code of Linux

Rules 2 and 3 do not weaken. MPL-2.0 does not let a GPL driver into
`components/`: the combination would have to be distributed under the GPL, which
is a regime this firmware does not want, and the kernel still has no module
loader and no `.ko` ABI (ADR 0025 §6). A driver ported from FreeBSD, OpenBSD or
NetBSD keeps its BSD notice; the port layer we write beside it
(`third_party/*/port/`, `ports/port_x86_64/programs/*.c`) becomes MPL - a larger
work of two licenses that needs the two notices it already has.

The direction MPL-2.0 opens is the one that was never closed: our code may be
taken into a GPL program, so a fork that adds a GPL-2.0-or-later driver and
ships the result as GPL is lawful - exactly as today under MIT, which permits
relicensing into anything. MPL does not narrow a downstream's freedom to make
the result more copyleft. SDL is not a component of this tree: it is a display
option of QEMU (`X86_DISPLAY=sdl`), a program of the host, zlib-licensed. A
Linux driver is GPL-2.0-only and stays out by rule 3 - the MPL would not help,
since such a file cannot meet §3.1 without the consent of its authors.

#### 2.4 The board, ESP-IDF, FreeRTOS and mbedTLS

The firmware is a larger work: our MPL beside ESP-IDF (Apache-2.0), FreeRTOS
(MIT) and the mbedTLS that ESP-IDF links (Apache-2.0). §3.3 permits exactly that
shape, and none of theirs becomes MPL; the Apache half brings its `NOTICE` duty,
which ESP-IDF already carries inside its build. What is new is the last clause
of §3.2: the board bundle has to say where the source of *our* part is, which is
the release tag. The x86-64 machine is the same with fewer parties.

#### 2.5 `third_party/`

The licenses there do not move. A vendored file is not Covered Software, because
Exhibit A's notice is not attached to it: it keeps the notice of its release,
and `docs/licensing.md` lists it. What becomes ours is what we wrote about it -
the `README.md` of each vendored directory, the `port/` layer, the
`CMakeLists.txt`, the `main` of a program in `ports/port_x86_64/programs/`. That
split already exists and rule 5 already states it; MPL makes it legal rather
than customary. A dual-licensed release (zstd is BSD-3-Clause or GPL-2.0) taken
under the permissive half stays permissive: the MPL around it does not launder
it.

#### 2.6 The SDK and the bundles

The SDK of 1.4 (`make sdk`, `docs/plan-1.4.md` M3) is a distribution twice: the
headers and the link script are source form (§3.1), `libmicroos_user.a` is
executable form (§3.2). Option B would make both MPL and require
`share/README.md` to name the source and the version of the library; Option C
keeps them MIT.

The bundles have a gap that exists today and that MPL would turn from an
oversight into a violation. `tools/bundle.sh` copies `LICENSE.spleen`,
`LICENSE.zlib` and the rest into the x86-64 archive, and `tools/x86_image.py`
writes `/etc/licenses/*` into the disk - but **the license of the tree itself
travels in neither**, and the archive carries no `LICENSE` of Micro OS at all.
MIT already asks that its notice accompany the software; §3.2 asks more, and in
a form a person can point at. The fix is small and owed either way: a `LICENSE`
in both bundles, a `/etc/licenses/microos` in the image, and a line in each
README naming the release the binaries came from.

#### 2.7 The community, the contribution flow, the files

Three facts make the migration cheaper now than it will ever be again. The
history has **one human author** under three e-mail addresses, so every line is
the owner's and a relicense needs nobody's permission. The tree has **no
per-file license notice** - `components/` and `ports/` begin with `#include`,
and the only SPDX lines outside `third_party/` are in the generated
`ports/port_x86_64/src/font_spleen.c` - so nothing has to be un-learned, but 443
`.c` and `.h` files outside `third_party/` (288 of them `.c`) and 186 scripts,
CMake files and documents have to gain one. And `CONTRIBUTING.md` has **no
sign-off** and no CLA.

That last point is the one to decide before, not after. A Developer Certificate
of Origin certifies the right to submit under the project's license; it is not a
grant of the right to relicense. A second move, or a dual license for a company,
needs either a CLA or the consent of every contributor - and once a second
person's code lands under MPL, that consent is the price of the next change. A
`-s` sign-off is worth adding for the record it creates.

## 3. What the migration would cost

If the answer is B or C, the work is, in the order it would land:

1. **`LICENSE`** - the MPL-2.0 text, verbatim, with the copyright line kept.
2. **A per-file notice** - `SPDX-License-Identifier: MPL-2.0` in every file the
   project wrote, and `make integrity` requires it. Without the notice a file is
   not Covered Software (§1.4), so the pass *is* the license.
3. **`docs/licensing.md`** - the opening paragraph, rule 1, a row for our own
   tree, the exception of Option C if taken, `MPL` named as copyleft for `pkg`.
4. **`README.md`, `README_ru.md`, `CHANGELOG.md`** and the wiki pages that say
   "MIT" for a program of the tree (`files.md`, `pkg.md`, `programs.md`); the
   landing page outside the tree.
5. **`tools/bundle.sh`, `tools/bundle/README.md`, `tools/x86_image.py`** - the
   `LICENSE` of the tree, `/etc/licenses/microos`, the source of that version.
6. **The SDK** (`share/README.md`) - the library version and its source, under
   Option B.
7. **`microos-progs`** - `licenses/NAME/MPL-2.0` beside the GPL text, the
   `license` fields, `sources/README.md`, and `MPL-` in the copyleft family of
   `tools/mkindex.py` and `components/pkg`.
8. **No `NOTICE`** is required by MPL - Apache-2.0 asks for one - but §3.4's
   notices must not be removed.

**Backwards compatibility** cannot be fixed by work. 1.0 to 1.3 are MIT,
published, and stay MIT forever; a fork of 1.3.0 under MIT is lawful and nothing
here reaches it. That has two faces. It is honest - nobody loses a right they
were given - and it is a hole: the most likely closed fork is a fork of the 1.3
the tree is proud of, and it owes nothing. MPL protects what comes after the
line.

Where the line falls is a smaller decision. The license is not one of the
contracts ADR 0011 froze - syscall numbers, `MOX1`, the volume format do not
move - so a minor is mechanically enough, and `1.4.0` or `1.5.0` with a section
of its own in the release notes is the clean way to say it. `2.0.0` is the
alternative: one sentence, "1.x is MIT, 2.x is MPL-2.0", at the price of waiting
for the next break of the ABI.

## 4. What it costs, and what it buys

**The price.**

* **The patent termination (§5.2) is a real cost of adoption.** A company with
  patents and a legal department reads "sue us over a patent and lose the
  copyright grant" as a question for counsel, and counsel's answer for a hobby
  kernel is usually "do not bother". That is the most common reason a small
  project stays permissive.
* **The simplicity is lost.** "MIT, do what you want" fits on a landing page;
  "MPL-2.0, file-level copyleft, keep the notice, tell us where the source is"
  is a paragraph, and a hobbyist reads the first and not the second.
* **The copyleft buys less than it looks** (§1): the natural closed use of this
  kernel is a new port and a new driver - new files - and MPL does not reach
  them.
* **Two regimes.** A maintainer who backports to 1.3.x keeps an MIT branch
  alive, and two licenses in two branches is a permanent small tax.
* **Every program image acquires an obligation** under Option B, including the
  ones a stranger writes for a closed product.

**What it buys.**

* **Improvements to our files come back** if they are distributed - the one
  thing a copyright holder can reasonably want, and the one MIT cannot ask for.
* **A patent grant and a patent peace**: contributors license what they own, and
  nobody who takes the code can sue a contributor over it without losing the
  grant (§2.1(b), §5.2). For a project that will accept code from strangers one
  day, that is the half of the license with lasting value.
* **A norm in a text**: a vendor who ships a closed derivative has to decide, in
  writing, instead of not thinking about it.
* **No damage to the architecture**: unlike Apache-2.0, MPL-2.0 composes with
  the GPL-2.0-or-later and GPL-3.0-or-later packages the system installs.

## 5. The alternatives

| Option | The core | A program linking the library | Verdict |
|---|---|---|---|
| MIT | nothing asked | nothing asked | honest while the risk is obscurity |
| Apache-2.0 | patent grant, no source duty | GPL-2.0-only cannot link | the patent half, dearer |
| MPL-2.0 | modified files published | §3.3 or §3.2 | the only copyleft that fits |
| AGPL-3.0 | network copyleft | breaks GPL-2.0-only | no: a kernel is not a service |
| BSL-1.1 | source-available, delayed | not OSI-approved | no: no product to protect |

**Apache-2.0 is not the cheap version of MPL** for this tree: it costs a
`NOTICE` and a per-file header as well, gives no file-level copyleft, and its
patent clause is less compatible with the existing GPL-2.0 packages than MPL's
§3.3 - so if the header price is paid at all, MPL is the better buy. **AGPL is
the wrong tool for a kernel**: its §13 is about a program used over a network,
which the firmware of a watch is not, and its incompatibility with GPL-2.0-only
would fracture the program set that ADR 0025 spent an ADR building. **BSL** is
not open source at all: a business instrument for a company with a product.

## Recommendation

**Stay MIT for 1.x, and decide again at 2.0** - unless one of the triggers below
has already fired. The reasons are the ones this tree can check: its realistic
risk today is obscurity, not a hidden fork; the copyleft would not reach the way
a commercial user would most likely take the code (a new port, new files); and
the one durable gain - patent peace - is worth less than the friction while
there is one author and no company across the table. That is Option A **plus the
notice fix of §2.6**, which is owed under MIT already.

**If the owner's goal is "the improvements come back", or a company is at the
table, or a second contributor is about to arrive: Option C, MPL-2.0 from the
next version, with the library of a program left MIT.** That pays the price once
and keeps ADR 0025's promise - any program, of any license, links the library -
without a new argument for each package. Option B, one text over the whole tree,
is defensible and simpler to explain, and §3.3 makes it lawful; it is chosen
when the owner would rather have one license than one exception.

Three triggers would make the answer MPL on the spot, and they are cheap to
watch: a product ships with modifications to `components/` that were never
published - the thing MPL is for; a company asks for an arrangement or an
exception, the moment the license stops being academic; the first outside
contributor's code lands, which closes the cheap window of §2.7.

**A checklist, if the answer is yes** (Option C, from the next version):

1. `LICENSE` becomes MPL-2.0, verbatim; the copyright line stays.
2. The SPDX pass, except `components/libc/user/`; `make integrity` requires it.
3. `docs/licensing.md`: rule 1, our tree a row, the library the exception.
4. The READMEs, the CHANGELOG, the wiki pages and the landing page.
5. `LICENSE` and a source pointer in both bundles; `/etc/licenses/microos`.
6. `microos-progs`: the texts, the `license` fields, `sources/README.md`.
7. `MPL-` in the copyleft family of `mkindex.py` and `pkg.c`, with a test.
8. `CONTRIBUTING.md`: `-s`, and a sentence on what it does not grant.
9. A release note: "from this version the tree is MPL-2.0; 1.x stays MIT".
10. No Exhibit B notice anywhere - it would cut the §3.3 compatibility of §1.
11. A lawyer reads §2.2 and §2.6 before the tag - not because the reading here
    is wrong, but because that is the sentence a distributor is held to.

## Consequences if accepted

**Good**

* The files this project writes cannot be improved in private and shipped: a
  distributed modification is published, and the source of our part travels
  with any binary of it.
* Contributors get a patent grant, and a suit against a contributor costs the
  plaintiff their license.
* The boundary of ADR 0025 survives: MPL-2.0 combines with the GPL-2.0 and
  GPL-3.0 programs the system installs.
* The rules become a text a lawyer can read and a person can obey, instead of a
  custom ("permissive only") that only the tree knows.

**Bad**

* 1.0-1.3 stay MIT, and a fork of any of them is lawful and silent: the
  copyleft protects only what comes after the line.
* Every binary distribution acquires a source-offer duty and every file a
  header; the bundles, the SDK and `microos-progs` each grow a paragraph.
* A company that would have embedded the kernel under MIT may ask its lawyer,
  and the answer may be no.
* The landing page loses its one-line license, and `docs/licensing.md` has to
  explain a tree whose own license is not the license of its library.

## What this ADR does not decide

* It does not change `LICENSE`, `docs/licensing.md`, `AGENTS.md` or a line of
  code. A status of `accepted` is permission to start, not the migration.
* It does not choose the version: the owner and the release notes do, and this
  page only says a minor is mechanically enough (ADR 0011) and a major is
  clearer.
* It is not legal advice. §2.2 (a static library in a GPL program), §2.6 (what a
  firmware bundle must say) and the effect of §5.2 on a company with patents are
  the three places where the answer belongs to a person with a legal education
  who has read the actual distribution.
* It does not decide the license of `microos-progs` as a repository: each
  package keeps the license of its program.
* It does not decide the fate of 1.x, of its backports or of a fork of it, and
  it creates no CLA. It registers no trademark either.
* It does not reopen ADR 0025: GPL code stays out of every binary this tree
  builds, whatever the license of the tree becomes.

## 6. What a change of the license would not buy (23 November 2026)

A question came back with a driver of a laptop as the reason: "we meet painful
limits, so after 2.0 the tree becomes MPL-2.0 and the limits go away". The
answer of the record is no: the limits are not the ones a license of *our*
files can lift, and this section says where each of them really lives.

* **A GPL driver stays impossible.** MPL-2.0 and GPL-2.0 are compatible in one
  direction only: MPL code may travel inside a GPL work (§3.3 and the secondary
  licenses), and GPL code may not be relicensed as MPL. A GPL driver linked
  into an MPL kernel is the same linking question as one linked into an MIT
  kernel, with the same answer: the whole becomes GPL. The license of the tree
  is not a lever here.
* **The technical wall is not the license either.** `iwlwifi` is a driver for
  `mac80211`: the state machine of 802.11 lives in a GPL subsystem of its own,
  and "a Linux-compatible API" means porting `netdev`, `skb`, `cfg80211`,
  `nl80211`, `mac80211`, RCU and the workqueues before the driver has one
  caller. The `iwlwifi` of a laptop is a project of the size of this system.
* **The firmware is proprietary at any license of ours.** The `.ucode` of a
  card ships with its own license, as a file outside the sources, in every
  distribution of every system that loads it.
* **What the BSD world gives for free.** `iwm(4)` of OpenBSD and FreeBSD
  speaks the same 8260/8265 and is BSD-2-Clause, and the `net80211` under it is
  BSD-2-Clause: a permissive path that this tree may take **today**, with no
  change of license. It is the answer to the pain, and it is already inside
  the rule of ADR 0025.

What a change of the license would buy is the thing §4 already measured: a
file-level copyleft over our own files, an express patent grant, and a work
that a GPL project may embed. Those are reasons to consider MPL-2.0 at 2.0;
they are not the reasons of a driver.

**The shape a GPL driver can have, if it is ever wanted:** a second build, not
a second license. The MIT tree stays the machine by default; `make gpl` would
build an image whose driver set is GPL, that image and its sources are
published under GPL-2.0 as a package of their own, and a person chooses which
one to install. That is how a permissive system lives beside copyleft code
without either lying about its license; it is a decision of its own, with an
ADR of its own, and this one does not take it.

