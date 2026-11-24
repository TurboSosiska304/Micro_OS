# The names of the releases

A version number says what a release promises to a program; a name says what it
promises to a person. The rule of this project: **the number is the contract
and the name is the greeting** - `kernel_version()`, the ABI and every file of
the tree carry the number and never the name, and the name lives in the title
of the release, in `docs/releases/`, on the site and in the tag message.

A tag is `vX.Y.Z`; a tag with a suffix is a candidate (`v2.0.0-rc1`) and
carries no name. The name is given to `X.Y.0` and to a patch only if the patch
is the release the name was waiting for (it is not, by default).

## The theme: lighthouses

**A lighthouse is a small light for a small machine.** It is not the coast, it
is not the ship and it is not the sea: it is a point of light that says "here
is the rock, and here is the way past it", it is built to be left alone for
years, and the famous ones have been rebuilt - Eddystone four times, each tower
learning from the one the storm took.

That is what this system is for: a small, patient machine that tells a person
where the rocks are. The ladder begins with 2.0, the release that makes the
machine trusted enough to be left alone with somebody's files.

| Version | Name | The tower | Why this release |
|---|---|---|---|
| **2.0.0** | **Eddystone** | the fourth tower of the Eddystone reef, 1882, still standing | the major that rebuilds the trust of the machine: a policy of the MMU, a real `fork`, accounts with a group and a `setuid` bit, a desktop that starts itself and a wire with an address nobody typed |
| 2.1.0 | Bell Rock | the oldest sea-washed tower of the world, 1811 | the release after a major: the defects a major leaves, the debts of `docs/debts.md`, and the first release that a stranger runs for a month |
| 2.2.0 | Skerryvore | the tallest of the family, 1844, built of stone that the sea cannot eat | durability: the page cache that a program can share, the second half of the journal, `fsync` |
| 2.3.0 | Fastnet | the granite of 1904, and the storm of 1979 that took the light and left the tower | the release of the storms: fault injection, a night of fuzzing, the errors that a disk and an allocator make |
| 2.4.0 | La Jument | the tower of the keeper in the wave, 1911 | the last minor of the line: the polish of the desktop, the applications of a person, and the guide |
| **3.0.0** | **Cordouan** | the oldest of the family, 1611, a landmark of a coast | the major of the stranger: an upgrade across it with the data, a suite of somebody else's programs, packages from the repository, and ten guarantees that a machine checks |

The ladder is a plan and not a promise of dates: a name belongs to the release
that takes it, and a release that moves keeps its name until it is cut (3.0
keeps Cordouan even if the ladder of 2.x is shorter or longer).

## The two other themes we did not take

* **the small islands** (Saba, Socotra, Ischia, Formentera, Pitcairn, Tristan da
  Cunha) - a small machine as an island, which is true, but an island is a
  place you reach and a release is a thing you use;
* **the hand tools of a workshop** (chisel, plane, rasp, brace) - the honesty of
  a tool, but a tool is used and a release is lived with.

## The rules of a name

* a name is **one word**, real, checkable, and not a trademark of a company;
* it is written in English in the title of the release and translated in the
  notes of a language that has the tower as a word of its own;
* it never appears in the ABI, in `kernel_version()`, in a path of the image or
  in a tag: the tag is `vX.Y.Z` and a script that reads it must not learn the
  name;
* the notes of a release open with the name and the sentence of what the number
  promises; the site shows the name under the version;
* a candidate carries no name until it is the release.
