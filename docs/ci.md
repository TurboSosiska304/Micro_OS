# Micro OS CI

> Russian version: [ci_ru.md](ci_ru.md)

The autobuild of the release candidate. Every push to `release_candidate`
builds the firmware for the ESP32 and checks the integrity of the repository
and of the flash bundle. A tag of the form `vX.Y.Z` publishes the bundle as
a GitHub release.

The tests of the code itself are not in this workflow: they belong to the
contributor, on their machine, until the first release gives them their own
CI/CD. What the CI speaks about is the artifact that goes onto the board.

The whole thing is one workflow: [.github/workflows/ci.yml](../.github/workflows/ci.yml).

## The triggers

| Event | Branches | What happens |
|-------|----------|--------------|
| push | `release_candidate`, `mainline` | the build and the checks |
| push | the tags `v*` | the same, and then the release |
| pull request | to `release_candidate`, `mainline` | the build and the checks |
| schedule | Mondays, 04:23 UTC | the same: the dependencies get old, the build has to keep working |
| manual | — | the same, from the Actions tab |

The scheduled run is taken from the default branch of the repository, so it
starts working only when the workflow reaches the default branch.

## The jobs

| Job | What it does | Notes |
|-----|--------------|-------|
| `integrity` | the integrity check of the repository and of the bundle | the whole history is fetched, `git fsck` has to pass |
| `firmware` | the firmware for the ESP32, the bundle, the check of the fresh binaries | the official `espressif/idf:v5.3` image |
| `release` | publishes the bundle as a GitHub release | only for the tags, after all the other jobs are green |

The build and the check are blocking: the branch is not green until the
firmware is built and the repository passes the integrity check.

### The tests are not here

The unit tests and the smoke test of the simulator are the tests of the code
itself, and they are run by the contributor on their machine, before a push
(see [Running it locally](#running-it-locally)). The unit tests are the tests
of `tests/`. The smoke test is the whole kernel at once: the simulator boots,
the subsystems come up, the shell answers the commands. It is the cheapest
end to end test of the kernel and it catches the things the unit tests
cannot see, a task that never wakes up for example.

They are out of the autobuild on purpose, and after the first release they
get their own CI/CD — an autonomous workflow, separate from this one.

### The firmware

The build runs inside the official image of the ESP-IDF, so the toolchain is
exactly the same as the one on the developer machine. The commands of the IDF
are on `PATH` only after `export.sh` — the image keeps the IDF in
`/opt/esp/idf` and says so in `IDF_PATH` — so every step of the build sources
it first:

```bash
. "$IDF_PATH/export.sh"
idf.py --version
idf.py set-target esp32
idf.py build
idf.py size
tools/bundle.sh build dist
```

The version of the ESP-IDF is pinned by the tag of the image
(`espressif/idf:v5.3`): it is the version of the branch `release-v5.3` of
ESP-IDF, the same one the project is written for.

After the build the bundle is collected and checked: the fresh binaries are
compared against `partitions.csv` and `sdkconfig.defaults`, the magic of the
image is compared against the format of the ESP-IDF, the sums are recalculated.

## The integrity check

`tools/integrity_check.py` is the heart of the check. It has no dependencies
beyond the Python standard library, so the same script runs on the laptop and
on the CI machine.

```bash
make integrity          # the whole check
tools/integrity_check.py --verbose --json report.json
```

| Group | What is checked |
|-------|-----------------|
| `bundle` | the files of `dist/` are there, the SHA-256 sums match, the flash script is executable and flashes every binary |
| `image` | the header of the ESP images: the magic `0xE9`, the chip of the ESP32, the alignment to 16 bytes, the number of the segments |
| `partition` | the compiled partition table against `partitions.csv`: the names, the types, the offsets and the sizes, plus the MD5 sum that the ESP-IDF writes into the table |
| `config` | the size of the flash of `sdkconfig.defaults` against the header of the images and against the partitions: nothing hangs out of the flash |
| `git` | `git fsck --strict`: the objects of the repository are intact |
| `sources` | the hygiene of the sources: UTF-8, LF, the final newline, no trailing whitespace, no tabs, the lines of the C sources are at most 100 characters long |
| `format` | the C sources are formatted (`clang-format --dry-run --Werror`) |
| `version` | the version of the kernel matches the tag (only for the releases) |

The check is strict on purpose: it is the guard against the bundle that does
not match the sources it was built from.

`dist/` is a product of the build, not a file of the repository. On a fresh
checkout the groups `bundle`, `image` and `partition` are skipped — there is
nothing to check — and the group `config` only speaks about
`sdkconfig.defaults`, which is in the sources. The firmware job of the CI
builds the bundle and checks the fresh binaries with the same script, with
`--skip-git --skip-format`: the objects of the repository and the formatting
of the sources are the business of the job `integrity`.

## The artifacts and the release

Every run uploads:

| Artifact | What is in it |
|----------|---------------|
| `integrity-report` | the report of the integrity check in JSON |
| `firmware` | `dist/` with the binaries, the sums and the flash script, plus `micro_os.elf` and `micro_os.map` for the debugging |

A tag of the form `vX.Y.Z` publishes a GitHub release with the bundle:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The release job checks that the version of the kernel in
`components/kernel/include/kernel/config.h` is the version of the tag, and
that the sums of the downloaded bundle match the binaries. The archives of the
Actions do not keep the permissions of the files, so the flash script gets
its executable bit back before it is published.

The symbols (`micro_os.elf`, `micro_os.map`) are in the release for the
debugging: they are not needed to flash the board.

## Running it locally

The CI does not run the tests yet: they are the work of the contributor, on
their machine. Everything at once:

```bash
make ci          # the build, the tests, the smoke test and the integrity check
```

The firmware is built by hand, because the ESP-IDF is not installed
everywhere:

```bash
make idf-build   # the firmware
make bundle      # the firmware into dist/ with the sums and the flash script
make integrity   # the check of the fresh bundle
```

## The pinned versions

| What | Version | Why |
|------|---------|-----|
| ESP-IDF | `v5.3` | the same one the project is written for, the same one the firmware is built with |
| clang-format | `23.1.3` | a different version formats the sources differently, and the check would fail for no reason |

The compiler of the host build is GCC, the same compiler the ESP-IDF uses.
