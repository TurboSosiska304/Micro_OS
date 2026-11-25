# The names of the releases

A version number says what a release promises to a program; a name says what it
promises to a person. The rule of this project: **the number is the contract
and the name is the greeting** - `kernel_version()`, the ABI and every file of
the tree carry the number and never the name, and the name lives in the title
of the release, in `docs/releases/`, on the site and in the tag message.

A tag is `vX.Y.Z`; a tag with a suffix is a candidate (`v2.0.0-rc1`) and
carries no name. The name is given to `X.Y.0`, and to a patch only if the patch
is the release the name was waiting for (it is not, by default).

## The theme: pictures

**A picture is a whole made of parts, and the parts are not the point.** A
Kandinsky composition is a dozen shapes that a person reads as one thing; a
Levitan sky is a minute of the day that the water agrees with; a Repin crowd is
eleven faces and one question. That is what a system is: a kernel, a library, a
desktop and a disk arranged so that a person sees a machine and not the parts.

The pictures are the Russian school, from the seascapes to the avant-garde,
because a name should be something a person can go and look at - and because a
picture, unlike a lighthouse, is not a thing on the coast: it is something
somebody made, which is what a release is.

| Version | Name | The picture | Why this release |
|---|---|---|---|
| **2.0.0** | **Composition** | Kandinsky, *Composition VII*, 1913 | the major that makes one whole of the parts: a policy of the MMU, a real `fork`, accounts with a group and a `setuid` bit, a desktop that starts itself and a wire with an address nobody typed - a dozen shapes read as one machine |
| 2.1.0 | Rooks | Savrasov, *The Rooks Have Come*, 1871 | the thaw after a major: the defects it left, the debts of `docs/debts.md`, and the first release a stranger runs for a month |
| 2.2.0 | Eternal Peace | Levitan, *Above the Eternal Peace*, 1894 | durability: the page cache a program can share, the second half of the journal, `fsync` - a machine left alone for years |
| 2.3.0 | Pompeii | Bryullov, *The Last Day of Pompeii*, 1833 | the storms: fault injection, a night of fuzzing, the errors a disk and an allocator make, and a machine that keeps its files |
| 2.4.0 | Peaches | Serov, *Girl with Peaches*, 1887 | the light of the last minor: the polish of the desktop, the applications of a person, the guide that a stranger reads |
| **3.0.0** | **Ninth Wave** | Aivazovsky, *The Ninth Wave*, 1850 | the morning after the storm: survivors on a mast at dawn - the major of the stranger, where an upgrade keeps the data, a rollback exists, and the packages arrive |
| 4.0.0 (reserved) | Black Square | Malevich, *Black Square*, 1915 | the release that ends the oldest promise instead of adding one: the board of the ESP32, which this system has carried since it existed and which a machine of 2.0 no longer needs. A reserved name, and the decision is the owner's, not the autopilot's |

The ladder is a plan and not a promise of dates: a name belongs to the release
that takes it, and a release that moves keeps its name until it is cut (3.0
keeps *Ninth Wave* even if the ladder of 2.x is shorter or longer).

## The themes we did not take

* **the lighthouses** (Eddystone, Bell Rock, Skerryvore, Fastnet, La Jument,
  Cordouan) - a small light for a small machine, and the first proposal; it was
  good, and a picture is better: a lighthouse is a thing that warns, and a
  release is a thing that is used and looked at;
* **the small islands** (Saba, Socotra, Ischia, Formentera, Pitcairn, Tristan da
  Cunha) - a small machine as an island, but an island is a place you reach;
* **the hand tools of a workshop** (chisel, plane, rasp, brace) - the honesty
  of a tool, but a tool is used and a release is lived with.

## The rules of a name

* a name is the **short name of a painting** - one word where the picture has
  one (`Rooks`, `Peaches`, `Pompeii`), the number of the picture where that is
  its name (`Composition VII` is `Composition`), and the painter is written
  beside it in the notes and on the site;
* it is real, checkable, in the public domain, and not a trademark of a
  company;
* it is written in English in the title of the release and translated in the
  notes of a language that has the picture as a word of its own;
* it never appears in the ABI, in `kernel_version()`, in a path of the image or
  in a tag: the tag is `vX.Y.Z` and a script that reads it must not learn the
  name;
* the notes of a release open with the name and the sentence of what the number
  promises; the site shows the name under the version;
* a candidate carries no name until it is the release.
