# mbedTLS in Micro OS

This directory is a copy of **Mbed TLS 3.6.2**, the TLS stack of the x86-64
port of Micro OS (ADR 0024 §3). The board keeps the copy that ESP-IDF already
carries and links none of this (ADR 0018, the compatibility port).

| | |
|---|---|
| Version | 3.6.2 |
| Upstream | https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.2 |
| Source | `mbedtls-3.6.2.tar.bz2` of that release, SHA-256 `8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca` |
| License | Apache-2.0 OR GPL-2.0-or-later, dual; Micro OS takes **Apache-2.0** |
| Vendored | 2026-11-22 |

## What is here

* `include/mbedtls/`, `include/psa/` — the public headers of the release, as
  they are upstream;
* `library/` — the sources of the library, as they are upstream;
* `LICENSE` — the upstream license file, which carries both grants;
* `config/microos_config.h` — **ours**: the single configuration of the
  build, a TLS client with no file system, sockets, threads or timing;
* `CMakeLists.txt` — **ours**: the object library `microos_mbedtls`.

Nothing under `include/` or `library/` was edited. An update is a new
release tarball unpacked over these two directories, with the version and the
sum above rewritten, and `make test` and `make x86-net-smoke` run again.

The upstream tests, programs, scripts, documentation and the sample
configuration are not vendored: the library is a dependency of the kernel
image, not a development tree.

## The configuration

`config/microos_config.h` replaces `mbedtls_config.h` and is selected with
`MBEDTLS_CONFIG_FILE="microos_config.h"` (the definition is PUBLIC on the
CMake target, so the sources of `components/net` see the same switches). It
enables TLS 1.2 and 1.3, X.509, ECDHE with ECDSA or RSA certificates,
RSA-PSS, AES-GCM/CCM, ChaCha20-Poly1305 and SHA-1/256/384/512. It disables
the file system (`MBEDTLS_FS_IO`), the socket layer (`MBEDTLS_NET_C`), the
threads (`MBEDTLS_THREADING_C`), the timing module (`MBEDTLS_TIMING_C`) and
the server side (`MBEDTLS_SSL_SRV_C`). The comments in that file are the
reason for each group.

## The platform layer

mbedTLS of a freestanding machine needs four things, and the kernel gives
them (ADR 0024 §3):

| mbedTLS asks for | Micro OS answers with |
|---|---|
| `calloc` / `free` | `kmm_alloc(KMM_PRIORITY_SYSTEM)` / `kmm_free`, set once with `mbedtls_platform_set_calloc_free()` in `components/net/src/tls.c` |
| `snprintf` | the formatter of the kernel's C library (`ports/port_x86_64/src/libc_shim.c`, `components/libc` on the board), set with `mbedtls_platform_set_snprintf()` |
| `time()` / `gmtime_r()` | `net_tls_time()` over `k_clock_now_unix()`, and `mbedtls_platform_gmtime_r()` of our own (UTC from the seconds of Unix) |
| entropy | `mbedtls_hardware_poll()`: `RDRAND` when CPUID says the processor has it, the timer (TSC jitter) otherwise |

The roots are not read by the library: `components/net/src/tls.c` reads
`/etc/ssl/certs.pem` through the VFS and hands the PEM buffer to
`mbedtls_x509_crt_parse()`. `MBEDTLS_PLATFORM_GMTIME_R_ALT` is part of the
configuration, so the library never reaches for a libc function that is not
there.

## The build

`CMakeLists.txt` compiles the sources as one OBJECT library with warnings off
(the code is not written in the style of this tree) and, on x86-64, at `-O2`:
at `-O3` with the compiler and the freestanding flags of the port the Finished
check of TLS 1.3 fails in `make x86-net-smoke`, while `-O2` and a debug build
pass. The comment of the CMake file and `docs/debts.md` say it; it is not a
setting of the protocol.

## Why this version

3.6 is the maintained branch of mbedTLS with TLS 1.3 and the X.509 name
checks of the current standards, and 3.6.2 is its latest patch release at the
moment of vendoring. The ESP-IDF v5.3 of this tree carries 3.6.0 under its own
configuration on the board; the two copies do not meet, because the board
does not compile this directory.
