# Contributing to Micro OS

Thanks for taking part. The kernel is written carefully, following rules that work
for humans as well as for automated checks.

## Before you commit

```bash
make test      # mandatory: the unit tests must pass
make format    # bring the code in line with the style (clang-format)
```

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

* `main` is always in a working state with a green `make test`.
* Features go to `feature/<what>`, bugs to `fix/<what>`.
* Every significant architectural change comes with an ADR in `docs/adr/`.

## Tests

* Unit tests live in `tests/`; the framework is our own minimal one
  (`tests/framework/ktest.h`) so the suite builds with nothing but a C compiler.
* Kernel tests must run on the host. If a test needs real ESP32 hardware, that is
  a signal that platform and portable code got mixed.
* A new kernel module comes with a new file in `tests/cases/`.

## Hardware

The quirks of the M5Stack StickC Plus2 (power model without a PMIC, pinout, I2C
addresses) are documented in
[docs/hardware/stickc-plus2.md](docs/hardware/stickc-plus2.md) — check it before
writing a driver.

## Translations

English is the canonical language of the repository. Russian translations live next
to the originals as `*_ru.md` and may lag behind the English version.
