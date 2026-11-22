# The tools of the tree

The scripts a person runs, beside the `make` targets. A tool that a test needs
is also an entry of `ctest`; a tool that is a host program (C, not Python) is
built into `build-host/tools/`.

| Tool | What it does |
|---|---|
| `console.py` | read the serial console of the board, with a duration |
| `stream_console.py` | the same console, interactive |
| `device-test.sh` | build, flash and read the console of the board, ending with `RESULT: PASS` |
| `sim_smoke.py` | boot the simulator and check its answers |
| `x86_smoke.py` | boot x86-64 in QEMU, check the transcript, the link address and the signature of the kernel |
| `x86_disk_smoke.py` | two boots on one disk image: a file written in the first is read in the second |
| `x86_net_smoke.py` | the network of the x86-64 machine against servers of the host |
| `x86_image.py` | write the disk image with `/bin` and `/etc` (calls `microos_mkfs`) |
| `mkfs_mfs.c` | the host tool that formats and fills a MicroFS volume (`microos_mkfs`) |
| `mkprog.sh` | turn the ELF of a program into a MOX1 image |
| `bundle.sh` | collect `dist/`: the board images, the x86-64 tarball, the signature and the sums |
| `microos_sign.c` | sign, verify and inspect the trailer of an image (ADR 0020) |
| `integrity_check.py` | the repository checks: format, git, hygiene, bundle, version |
| `sram_budget.py`, `coverage_report.py` | the static budget and the coverage report |
| `soak.py`, `host_stress.c`, `board_soak.sh` | the campaign of 1.0: the mixed load of the 600 seconds |

## The signature of an image (ADR 0020)

`microos_sign` is built by the host and by the x86-64 project:

```bash
build-host/tools/microos_sign keygen --key dev.key --pub dev.pub
build-host/tools/microos_sign sign   --key dev.key --in kernel.raw --out kernel.bin --sig kernel.sig
build-host/tools/microos_sign verify --pub dev.pub kernel.bin
build-host/tools/microos_sign info   kernel.bin
```

The image is the payload and 120 bytes after it: `MOSIGN01`, the version, the
algorithm, the size, the SHA-256 of the payload and an Ed25519 signature of
those fields. `verify` reads the trailer at the end of the file;
`--trailer FILE` verifies a payload with the detached copy of the trailer.

The private key is not in this tree. `MICROOS_SIGN_KEY` names one for
`make x86-build`, `make idf-build` and `make bundle`; without it the build
generates a key in its directory (`build-x86/signing.key`,
`build/signing.key`) and the loader or the firmware carries its public half.
The public key of a build travels as `signing.pub` (and `microos-sign.pub` in
`dist/`).

`make x86-smoke` proves the x86-64 boot check from both sides: the good image
prints `Micro OS: image signature ok (sha-256, ed25519)`, and a changed byte,
a missing trailer and a foreign key each stop with `Micro OS: the image is
refused` before the kernel prints its banner. With
`MICRO_OS_REQUIRE_SIGNATURE=OFF` (`make x86-open-build`) the same bytes boot
after `Micro OS: unsigned or damaged image: not guaranteed`.

`make bundle` signs `micro_os.bin` and writes `micro_os.bin.sig`; `flash.sh`
puts it at `0x14F000`, the last 4 KiB of `ota_0`, where `app_main` reads it
(an update writes the trailer at the end of `ota_1`, `0x28F000`, ADR 0022). The board's check is electronic and its policy is the
Kconfig `MICRO_OS_REQUIRE_SIGNATURE` (ADR 0020, sections 6 and 7).
