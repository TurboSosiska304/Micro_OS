# Licenses

Micro OS itself is MIT: see [LICENSE](../LICENSE). A copy of a third-party
component keeps its own license in its directory, and the terms of that
component are the ones of its license, not the MIT of the tree.

## The components of a distribution

| Component | Where | License | Why it is there |
|---|---|---|---|
| Micro OS | the tree | MIT | the kernel, the ports, the shell, the library |
| PCRE2 10.42 | `third_party/pcre2` | BSD-3-Clause WITH PCRE2-exception | `/bin/pcre2grep` of x86-64 |

There is one third-party component today. The board's firmware does not
contain it: `/bin/pcre2grep` is a program of the x86-64 image, and a program
is a file of the disk, not a line of the application.

## PCRE2

The full text is [`third_party/pcre2/LICENSE`](../third_party/pcre2/LICENSE).
It is the license of the release, with the exception for binary
redistributions that names the copyright holders. In short:

* the source and the binary may be redistributed, with the copyright notice,
  the list of conditions and the disclaimer;
* the name of the University of Cambridge or of a contributor is not used to
  endorse a derived product;
* the "PCRE2-exception" of the SPDX identifier covers the binary form of a
  product that uses the library without shipping its source; the exception
  does not remove the three conditions above.

The copy travels with the machine that carries the program: `make x86-disk`
writes the same file to `/etc/licenses/pcre2` of the MicroFS volume, and the
smoke test reads the directory. `tools/bundle.sh` copies it to the bundle of
the x86-64 port as `LICENSE.pcre2`, so a disk and an archive name the terms
of every byte they carry.

The vendored source is unmodified; `third_party/pcre2/README.md` records the
release, the archive digest, the configuration and the two optimization
passes that the image leaves out. A change there is a change of a
third-party component: it needs the license to stay, a note in that README,
and a test that still passes.

## A component that arrives later

A new vendored directory follows the shape of `third_party/pcre2`:

1. the upstream license under the component's name (`LICENSE`), unchanged;
2. a `README.md` with the version, the source URL, the digest of the archive
   and every local change;
3. the license in the bundle and, for a machine that carries the component
   at run time, a file under `/etc/licenses/`;
4. a row in this table.

`make integrity` checks the hygiene of the tree, not the presence of a
license: the review of a vendored directory is a duty of the person who adds
it.
