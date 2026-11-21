# Contributing to Micro OS

Thanks for taking part. The kernel is written carefully, following rules that work
for humans as well as for automated checks.

## Before you commit

```bash
make test      # mandatory: the unit tests must pass
make format    # bring the code in line with the style (clang-format)
```

Before you push:

```bash
make ci        # the build, the tests, the smoke test, the integrity check
```

`make ci` is the list of checks before a push: it builds, runs the unit
tests, runs the smoke test of the simulator and checks the integrity of the
repository — the git objects, the hygiene of the sources and their
formatting. The workflows of the cloud repeat this list on every push to
`mainline` and `release_candidate` (and add valgrind and the build of the
firmware), so a push is checked again there. The check of the bundle in
`dist/` joins in when the firmware is built (`make bundle`), and in the
cloud — in the job that built it.

If you touched the ESP32 port, additionally:

```bash
make idf-build
```

## Code style

* C11, no C++, no GNU extensions in the portable kernel (`components/kernel`).
* 4 spaces, maximum line length 100 characters.
* `-Wall -Wextra -Werror` are enabled: compiler warnings are errors.
* Naming: `k_` for the kernel (`klog`, `k_port_write`), `board_` for the board
  description, `drv_` for drivers, `port_` for the platform layer.
* Header files use `MICRO_OS_..._H` include guards.
* Never call libc `printf`/`malloc` directly from the kernel: use `klog`,
  `kmm_alloc`, `k_port_write`.
* Global variables need a `g_` prefix and a unit suffix (`_s`, `_ms`, `_kb`).

## Commits

Messages follow the Linux style and name the area:

```
kernel: add task priority inheritance
vfs: fix path lookup for trailing slash
drivers: fix i2c timeout on mpu6886
docs: describe the stickc plus2 power model
```

Rules:

* one logical unit of change per commit;
* first line up to 72 characters, no trailing dot;
* explain *why* in the body, not *what* — the diff already shows that.

## Branches and releases

* `release_candidate` is the branch of the release candidate: every push to it
  runs the checks in the cloud, as does `mainline`. A tag of the form
  `vX.Y.Z` builds the firmware and publishes the bundle as a GitHub release;
  a tag with a dash (`vX.Y.Z-pre`) is a pre-release. The tests are run by
  the contributor locally and repeated in the cloud.
* `mainline` is always in a working state with a green `make ci`.
* Features go to `feature/<what>`, bugs to `fix/<what>`.
* Every significant architectural change comes with an ADR in `docs/adr/`.

### Versions and the changelog

The rule is [ADR 0011](docs/adr/0011-versioning-and-1.0.md), and every
release has a section in [CHANGELOG.md](CHANGELOG.md).

* **Before 1.0 the minor number is a phase, not a compatibility promise.**
  0.5, 0.6, 0.7 may break the syscall table, the image format or the volume
  when the release notes and the migration guide say so.
* **1.0 freezes the contracts.** After `1.0.0` the numbers and the meaning of
  the system calls, MOX1 and the MicroFS volume change only by adding; a
  breaking change is `2.0.0`. A new call takes the next free number, and a
  number is never reused.
* A release candidate (`vX.Y.Z-rcN`) is cut before a major; only fixes are
  allowed after it. A pre-release tag with a dash must match
  `kernel_version()` in `components/kernel/include/kernel/config.h`.
* The changelog says what changed, not what is planned: a feature is written
  there when its commit is in the tag, and a gate that is still open stays
  open in [`docs/releases/1.0.md`](docs/releases/1.0.md).

## Tests

* Unit tests live in `tests/`; the framework is our own minimal one
  (`tests/framework/ktest.h`) so the suite builds with nothing but a C compiler.
* Kernel tests must run on the host. If a test needs real ESP32 hardware, that is
  a signal that platform and portable code got mixed.
* A new kernel module comes with a new file in `tests/cases/`.
* `make test-tsan` runs the same tests under the race detector; a race it finds
  is a defect of the kernel or of the test, never noise.
* The x86-64 port is checked by `make x86-smoke` (it needs QEMU); a change of the
  contract of the port must keep it booting.

## Hardware

The quirks of the M5Stack StickC Plus2 (power model without a PMIC, pinout, I2C
addresses) are documented in
[docs/ports/esp32/hardware.md](docs/ports/esp32/hardware.md) — check it before
writing a driver. The board is one port of several: a change of the board or
of a driver must keep the other builds green (`make ci`, `make x86-smoke`),
and a change of the contract of the port is a change for every port — bump the
version in `kernel/port/info.h` and make all of them answer.

## Translations

English is the canonical language of the repository. Russian translations live next
to the originals as `*_ru.md` and may lag behind the English version.
