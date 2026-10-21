# Phase 0 - The skeleton

The kernel of Micro OS for the M5Stack StickC Plus2 starts with a project
that can be built, tested and flashed.

## What is there

* The kernel core is **portable**: plain C11, no ESP-IDF in it. Everything
  platform dependent is behind `kernel/port.h`.
* Two builds from the same sources: the firmware (ESP-IDF) and the host build
  (a simulator plus the unit tests). The host build runs in a second, so the
  tests are cheap.
* The log with the levels and a ring buffer, the panic handler, the version
  and the build identification.
* The description of the board: the SoC, the flash, the PSRAM, the display,
  the IMU, the RTC, the power.
* The boot sequence: the subsystems register themselves with a level and are
  initialised in the order of the levels, like the initcalls of Linux.

## The mistakes that cost time

* **The `build/` directory ended up in the history.** The rule in
  `.gitignore` was commented out by accident while the rules for the `bild/`
  folder were written. The repository grew to 34 MB of build output. The
  directory is ignored again, the history is not rewritten (that would need
  a force push).
* **A silent replacement.** Several times a patch of the source did not
  apply and the file was written unchanged. A call of `sched_init()` was
  renamed to `k_sched_init()`, the replacement did not match, and the kernel
  booted without `kmm_init()` and `ksys_init()` - the host tests hid it,
  because they called the initialisers by hand. **Every replacement is
  checked now.**

## What is worth remembering

* The unit tests have to run on the host. If a test needs the real board,
  the code mixes the platform with the portable part.
* The names of the kernel get the prefix `k_` (`k_sched_yield`), because
  `sched_yield()` is a function of POSIX and the two collided.
