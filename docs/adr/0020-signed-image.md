# ADR 0020 - A signed image

* Status: accepted, with the board's boot check deferred
* Date: 2026-11-22
* Supersedes: nothing
* Related: ADR 0004 (the port), ADR 0007 (the x86-64 boot), ADR 0011 (the
  gate of 1.0), ADR 0018 (programs of the board)

## Context

The gate of 1.0 asks for *images with a checksum and a signature that the
boot checks*. A checksum alone answers "was this file copied whole"; it says
nothing about where the file came from, and a checksum stored next to the
image is changed with the image by anyone who can write the medium. A
signature answers both, as long as the public key is in the boot path and not
in the file being checked.

The machines of this tree are different at the boot:

* **x86-64** owns its bootloader (`ports/port_x86_64/loader32.S`). It is a
  multiboot image QEMU loads at one megabyte, and it copies the kernel module
  (`microos64.bin`) to 16 MiB, builds the page tables and enters the long
  mode. There is a natural place for a check: before the copy.
* **ESP32** is booted by the ROM and then by the ESP-IDF second-stage
  bootloader. The ROM verifies the bootloader, and the bootloader verifies the
  application's checksum; neither verifies a signature unless *secure boot v2*
  is enabled. Secure boot v2 burns eFuses and is a one-way change of the
  board.
* **host** and **null** are not machines that boot an image; their "firmware"
  is a process of Linux.

The threat model is the one of a device on a desk, not of a phone with a
secure element:

**In scope**

* a corrupted image: a bad flash, a truncated file, a byte changed in a
  download, a build that wrote the wrong binary;
* an image of another build or another project: the bytes are intact but they
  are not Micro OS, or not this version of it;
* an image signed by a key that is not the one the boot trusts.

**Out of scope**

* a local attacker with write access to the flash, the bootloader or the
  private key: without an MMU, a measured boot or a write-protected boot
  medium, such an attacker replaces the checker as easily as the checked;
* rollback: an old, correctly signed image is a valid image. A counter that
  would refuse it is a decision of a later release;
* confidentiality: a signature does not hide the image;
* a compromised build machine: the key signs whatever that machine produces.

## Decision

### 1. The envelope: a trailer after the payload

A signed image is a payload followed by 120 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `MOSIGN01` |
| 8 | 4 | version, little endian (1) |
| 12 | 4 | algorithm, little endian (0 = SHA-256 + Ed25519) |
| 16 | 8 | the size of the payload, little endian |
| 24 | 32 | SHA-256 of the payload |
| 56 | 64 | Ed25519 over bytes 0..55 |

The signature covers the header and the digest, so a trailer cannot be moved
to another size or another algorithm; the digest covers every byte before the
trailer. The payload keeps its own format: the trailer is an envelope of a
whole file and does not change MOX1, the MicroFS volume or the ESP-IDF image.
The parser is one file, `ports/port_x86_64/loader/sign_image.c`, compiled into
the tool, the loader and the host tests; a second parser would be a second
answer to "is this image good".

### 2. The algorithm: SHA-256 and Ed25519

SHA-256 (FIPS 180-4) is the hash and Ed25519 (RFC 8032) is the signature.
Ed25519 was chosen over ECDSA over P-256 for three reasons:

* the verifier is small and has no allocation: the arithmetic of TweetNaCl
  (public domain) is a few kilobytes of portable C, which fits a 32-bit
  loader and has no libc in it;
* a signature is 64 bytes and a public key 32, against a DER structure and a
  curve point of ECDSA;
* there is no nonce to get wrong at signing time: the nonce is derived from
  the secret and the message.

mbedTLS on the ESP32 would be the alternative, and it is the natural choice
for a verifier that already has the library. It is not on the x86-64 boot
path, where there is no libc at all, and a second implementation of the same
check on the two machines is a second thing to test. The 32-bit arithmetic of
TweetNaCl needs only integer operations; the loader is compiled with
`-mno-sse -mno-mmx -march=i386`.

### 3. What is signed

* **the kernel image of x86-64** (`microos64.bin`): the loader verifies it at
  the boot, before a byte of the kernel runs. This is the boot check of 1.0.
* **the merged board bundle** (`MicroOS-<version>.bin`): `tools/bundle.sh`
  appends the trailer and writes `<image>.sig` and `checksums.txt`. This is a
  signature of the *distribution*, checked by a person or a tool before
  flashing; the board's boot chain does not verify it (see section 6).
* **not a `.mox` program**, not a MicroFS volume and not `/etc`. A program is
  a file inside a volume, and the volume is data of a machine, not an image
  of the system: signing every file would need a field in the record format,
  a key in the kernel and a policy for the files a user writes. It is a
  project of its own, recorded in the debts.

### 4. Where the key lives

The **public** key is part of the boot path: `microos_sign pubkey --header`
writes `sign_public_key.h`, and CMake compiles it into `loader32.elf`. The
loader carries the key, so an image cannot bring one of its own. The public
key of the x86-64 bundle is also shipped as `microos-sign.pub` inside the
tarball.

The **private** key is never in this tree. A release names it with
`MICROOS_SIGN_KEY` (a path outside the repository); the release workflow of
GitHub receives the key material as a secret and writes it to a file before
the build, because a secret is a value and not a path. A build without the
variable generates a key in its build directory (`build-x86/signing.key`) and
signs the image with that. The second case is the development loop and the
smoke test: the check is real, and the key is not a secret because it signs
nothing that leaves the machine. `make x86-build` passes `MICROOS_SIGN_KEY` to
CMake, and `make bundle` uses the same key for the board bundle when it is
set.

There is one key of the boot in 1.0. A key rotation means a new loader; a
list of several public keys and a key identifier in the trailer are the
natural 1.x extension, and the version and algorithm fields of the trailer
leave room for it.

### 5. Where the check runs on x86-64

`loader32.S` calls `loader_verify_image()` — 32-bit C, `-ffreestanding`, no
libc — after the `.bss` is cleared and before the page tables are built:

1. the trailer is at the end of the multiboot module; the magic, version,
   algorithm and payload size are checked;
2. SHA-256 of the payload is compared with the digest in the trailer;
3. Ed25519 verifies the first 56 bytes with the key compiled into the loader;
4. on a refusal the loader prints `Micro OS: the image is refused: <reason>`
   to COM1 and the bottom row of the VGA text screen, and halts. The kernel is
   not copied, the long mode is not entered, and no byte of the image runs.
   On success it prints one line and the boot continues.

This is a boot check in the only sense that matters: the checker runs before
the checked code and cannot be influenced by it. `make x86-smoke` proves both
halves: the normal boot prints the acceptance line, and a byte changed in the
kernel, a kernel without a trailer and a kernel signed by another key each
stop before `board: qemu_x86_64` appears.

### 6. The ESP32: what is real, and what is not

The ROM verifies the second-stage bootloader; the ESP-IDF bootloader verifies
the application image (its segments, checksum and SHA-256). Neither is a
signature. The ways to add one:

* **secure boot v2** — the supported path. It burns eFuses (the digest of the
  key, the secure-boot enable bit), the board cannot be returned to a
  development build without a signed image, and the key ceremony is a release
  operation. It is irreversible and it is **not** enabled by this tree.
* **a custom bootloader component** (`bootloader_components/`, the
  `bootloader_after_init` hook of ESP-IDF 5.3) could verify our trailer before
  `bootloader_utility_load_boot_image`. It does not burn an eFuse, but the
  bootloader partition is 28 KB (`0x1000`–`0x8000`) and the Ed25519 verifier
  and SHA-512 do not fit beside the IDF bootloader. It would need a new
  partition layout, which ADR 0011 keeps out of 1.0 because it also costs the
  volume.
* **a check in `app_main`** would run after the bootloader has already copied
  and entered the application. It is a self-check: it can notice a damaged
  file, it cannot stop the damaged code from having run. It is not a boot
  check and this tree does not call it one.

So the signature of the board bundle is a distribution check in 1.0. A
boot-checked board image is the first item of 1.1, either with secure boot v2
and a documented key ceremony or with the partition layout of a custom
bootloader. `docs/releases/1.0.md` keeps the gate line open and says exactly
this.

## Consequences

**Good**

* A corrupted or foreign x86-64 kernel does not boot, and the reason is on
  the console.
* The verifier is one small portable file, shared by the tool, the loader and
  the tests, with the RFC 8032 and FIPS 180-4 vectors.
* The keys are separate from the tree: a release key is a deployment secret,
  and the tree contains only the public half.
* The trailer is an envelope: no ABI, no MOX1, no volume format changes, so
  the check can be added to another machine without touching the payload.

**Bad**

* The board's boot chain still trusts a checksum, and the gate line is not
  closed for it. The wiki and the release notes say so.
* A key in the loader is a key that a new loader has to replace; rotation is
  not online.
* The image of x86-64 grows by 120 bytes, and a build that signs with a fresh
  key is not byte-for-byte reproducible unless the key is fixed
  (`MICROOS_SIGN_KEY`).
* Signing does not stop a local attacker from replacing the loader; there is
  no measured boot on either machine.

## Alternatives considered

* **A checksum only** (SHA-256 in a trailer, no signature): refused. It
  detects a bad copy, not another image with its own checksum, and the gate
  asks for a signature.
* **ECDSA P-256 over mbedTLS everywhere**: the ESP32 already links mbedTLS,
  but x86-64 has no libc under the boot loader, and the verifier would be
  larger and less self-contained.
* **Sign every MOX1 program**: refused for 1.0. It changes the program format
  and the syscall edge, and the program arrives through a filesystem the user
  can write; the signature of the kernel image is the boundary of the system.
* **Secure boot v2 at once**: refused. It burns eFuses and the release has no
  key ceremony; an irreversible change of every board on a desk is not a
  prototype decision.

## Open items

1. The board's boot check: secure boot v2 with a key ceremony, or a custom
   bootloader and a partition layout that fits the verifier (1.1).
2. A release secret in CI: `.github/workflows/release.yml` reads
   `MICROOS_SIGN_KEY` from a secret, and the secret does not exist yet. Until
   it does, the bundle is signed with the key of the local x86-64 build.
3. A rollback counter and a version policy: an old signed image boots today.
4. Signatures of programs and volumes, and a policy for `execve`.
5. A second public key and a key identifier, for a rotation without a new
   loader for every key.
