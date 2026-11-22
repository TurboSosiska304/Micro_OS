# ADR 0020 - A signed image

* Status: accepted; the board's check is an electronic check of the boot
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
* rollback by default: an old, correctly signed image is a valid image. The
  board has an opt-in floor in its NVS (`MICRO_OS_ANTIROLLBACK`), and x86-64
  has no persistent store and therefore no floor;
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
* **the application of the board** (`micro_os.bin`): `tools/bundle.sh` writes
  `micro_os.bin.sig`, a 120-byte trailer over exactly the bytes of the app,
  and `flash.sh` writes it at the last 4 KiB of the factory partition
  (`0x20F000`). The board verifies it at the beginning of `app_main`, before
  the filesystem and the shell (section 6). In 1.1 the board has two
  application slots (ADR 0022): the trailer is the last 4 KiB of the slot the
  board is running (`0x12F000` for `ota_0`, `0x24F000` for `ota_1`), and
  `tools/ota_usb.py` writes it with the image of an update.
* **the merged board bundle** (`MicroOS-<version>.bin`): `tools/bundle.sh`
  appends the trailer and writes `<image>.sig` and `checksums.txt`. This is a
  signature of the *distribution*, checked by a person or a tool before
  flashing; the board does not read this file, it reads the app trailer.
* **not a `.mox` program**, not a MicroFS volume and not `/etc`. A program is
  a file inside a volume, and the volume is data of a machine, not an image
  of the system: signing every file would need a field in the record format,
  a key in the kernel and a policy for the files a user writes. It is a
  project of its own, recorded in the debts.

### 4. Where the key lives

The **public** key is part of the boot path: `microos_sign pubkey --header`
writes `sign_public_key.h`, and CMake compiles it into `loader32.elf` and into
the ESP32 firmware. Both checkers carry the key, so an image cannot bring one
of its own. The public key is also shipped as `microos-sign.pub` inside the
bundle of each machine; `make idf-build` and `tools/bundle.sh` use the same
`MICROOS_SIGN_KEY`, or the development key in the build directory.

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

### 6. The ESP32: the electronic check of the application

The ROM verifies the second-stage bootloader; the ESP-IDF bootloader verifies
the application image (its segments, checksum and SHA-256). Neither is a
signature. The ways to add one are still the three below, and the third is the
one this release takes:

* **secure boot v2** — the supported path of Espressif. It burns eFuses (the
  digest of the key, the secure-boot enable bit), the board cannot be returned
  to a development build without a signed image, and the key ceremony is a
  release operation. It is irreversible and it is **not** enabled here. A
  board that wants it follows [the manual of ESP-IDF] and accepts the loss of
  the development loop.
* **a custom bootloader component** (`bootloader_components/`, the
  `bootloader_after_init` hook of ESP-IDF 5.3) could verify the trailer before
  `bootloader_utility_load_boot_image`. It does not burn an eFuse, but the
  bootloader partition is 28 KB (`0x1000`–`0x8000`) and Ed25519 and SHA-512 do
  not fit beside the IDF bootloader. It would need a new partition layout,
  which ADR 0011 keeps out of 1.0 because it also costs the volume.
* **a check in `app_main`**, which this release has. The first thing the
  application does, before it mounts MicroFS or starts the shell, is read its
  own factory partition: the ESP header gives the length of the image and the
  `esp_app_desc_t` gives its version, and the trailer is the last 4 KiB of
  the partition (`partition_end - 0x1000`, not after the payload). The check
  hashes the app bytes in chunks with SHA-256, compares the digest and
  verifies Ed25519 with the public key compiled into the firmware. The flash
  layout is unchanged and no eFuse is burned.

The third is **not** a boot-chain check and this tree does not pretend it is:
the ROM and the bootloader have already copied and entered the application,
so its first instructions (the static constructors and the first lines of
`app_main`) ran before the sentence appears. It is an electronic check of the
boot: it tells a person that the image is not the one that was signed, it
refuses to go on when the build requires a signature, and it does so before
the filesystem and the shell. It does not protect against a local attacker
who can replace the application or its checker, and it is not secure boot.

*Update (1.1.0).* The board has two application slots and an otadata that
selects one (ADR 0022). The check reads the trailer of the running slot, and
a refusal with the policy on marks that slot invalid and restarts the board
into the other one (`esp_ota_mark_app_invalid_rollback_and_reboot()`) instead
of only halting; with no other image to run it halts, as section 7 says. The
envelope, the algorithm, the key and the sentences of the policy do not
change.

The trailer is over the bytes of `micro_os.bin` exactly, and
`tools/bundle.sh` also leaves the combined `MicroOS-<version>.bin.sig` for a
person or a tool: that one covers the merged distribution image and is not
what the board reads. `tools/bundle.sh` checks that the app and the 4 KiB
reserve fit the factory partition; the ESP-IDF build checks the same number
before it writes the image, and `flash.sh` writes the trailer at `0x20F000`.

### 7. The policy: a setting, not a wall

A signature that stops a board is a wall. The decision of this release is
that the wall is a setting: the image is checked on both machines, but a
person who knows what the check costs may boot an image that did not pass.

* **x86-64** has a CMake option `MICRO_OS_REQUIRE_SIGNATURE`, **on** by
  default. On, the loader refuses a damaged, unsigned or foreign-key image
  before it copies a byte. Off, the loader still verifies the trailer and
  still prints the reason; it then prints
  `Micro OS: unsigned or damaged image: not guaranteed` and loads the kernel.
  `make x86-open-build` builds the second policy and `make x86-smoke` boots
  an unsigned and a damaged kernel on it, beside the refusals of the strict
  loader.
* **ESP32** has the Kconfig option `MICRO_OS_REQUIRE_SIGNATURE`, **y** by
  default, with the same two answers. On, a missing or wrong trailer prints
  `boot: the image is refused: <reason>` and the board halts before the
  filesystem and the shell. Off, the line
  `boot: unsigned or damaged image: not guaranteed` goes to the console and
  the boot continues. The signal is electronic and the line says so.
* **Anti-rollback** is a second setting, `MICRO_OS_ANTIROLLBACK`, **n** by
  default, and it exists only on the board because only the board has a store
  that survives a flash of the application. The version comes from
  `esp_app_desc_t.version`; the boot keeps the newest version it has seen in
  its own NVS namespace (`microos_boot`, the key `min_version`). With the
  flag on, an image lower than the stored one is refused with both versions
  on the console; with the flag off it is a warning. A newer version is
  recorded after the signature passed. A store that cannot be read or written
  is a warning and not a stopped board: a damaged NVS is not proof of a
  rollback, and the signature is still the boundary. This is not the
  anti-rollback of secure boot v2: a person who can flash the board can erase
  the NVS. x86-64 has no persistent store, so a rollback there is allowed and
  there is no flag; the version in the trailer leaves room for a floor when
  the port has a store.
* **What the setting does not make safe**: an image that did not pass is run
  because a person chose it; the board has already entered the application
  when its check runs; a damaged image may have executed its first
  instructions; and a signature says nothing about a local attacker with
  write access to the flash. The console sentence is the whole promise:
  *not guaranteed*.

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

* The board's check runs in the application, after the bootloader has entered
  it; the ROM still trusts a checksum for the boot chain. The check is
  electronic, and the wiki and the release notes say so.
* The anti-rollback of the board is in the NVS: it is not the eFuse counter of
  secure boot, and a person who can flash the board can erase it.
* A key in the loader and in the firmware is a key that a new build has to
  replace; rotation is not online.
* The image of x86-64 grows by 120 bytes, and a build that signs with a fresh
  key is not byte-for-byte reproducible unless the key is fixed
  (`MICROOS_SIGN_KEY`).
* Signing does not stop a local attacker from replacing the loader or the
  firmware; there is no measured boot on either machine.

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

1. The board's boot-chain check: secure boot v2 with a key ceremony, or a
   custom bootloader and a partition layout that fits the verifier. The
   electronic check of `app_main` is the answer of 1.0 for a board whose flash
   layout is frozen, not the end of the question.
2. A release secret in CI: `.github/workflows/release.yml` reads
   `MICROOS_SIGN_KEY` from a secret. A build without it uses a development
   key, and the bundle says so.
3. Anti-rollback on x86-64: the trailer has a version, but the port has no
   store that survives a reset, so there is no floor and no flag.
4. Signatures of programs and volumes, and a policy for `execve`.
5. A second public key and a key identifier, for a rotation without a new
   loader for every key.
6. A store of the boot check that a person with a flash writer cannot erase:
   it is the eFuse of secure boot v2, and it is a release operation.
