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
| `release_gate.sh` | the gate of a release in one command: every smoke, the two sessions of 600 seconds, the board, `integrity --expect-tag` and a release from an empty directory (`make release-gate`) |
| `x86_smoke.py` | boot x86-64 in QEMU, check the transcript, the link address and the signature of the kernel |
| `x86_vga_smoke.py` | the framebuffer, the text mode and the keyboard, with a screenshot and no display (`make x86-vga-smoke`) |
| `x86_usb_smoke.py` | a keyboard, a stick and a wire of the USB on the xHCI controller of a machine with no PS/2 (ADR 0040, `make usb-smoke`) |
| `x86_power_smoke.py` | `shutdown` and `reboot` return the machine through ACPI S5, and the same image boots again with its file (ADR 0050, `make power-smoke`) |
| `x86_mmu_smoke.py` | the policy of a page (ADR 0052): the attacks of W^X, the kernel, a stray page, the limit of a task, the stack that grows and a copy that sleeps, on `-cpu qemu64,+smep,+smap` (`make mmu-smoke`) |
| `x86_fork_smoke.py` | the fork of a program of 64 MiB (ADR 0057): the pages are shared and copied on the first write, the time of the call and the count of the pages (13 checks) |
| `x86_disk_smoke.py` | two boots on one disk image: a file written in the first is read in the second |
| `x86_root_smoke.py` | two roots of one machine: the MicroFS and the ext2 image answer the same commands, and FAT32 mounts at `/data` (`make x86-root-smoke`) |
| `x86_fs_smoke.py` | `rename` and `rmdir` through a program, and the `fsck` of a broken ext2 image (`make x86-fs-smoke`) |
| `x86_mfs_smoke.py` | a volume that loses the power in the middle of a write of 16 MiB, and the `fsck` of the second boot (ADR 0038, `make x86-mfs-smoke`) |
| `x86_cut_smoke.py` | the gate of a file that survives a cut: twenty interruptions of one write, the journal replayed or the draft discarded (ADR 0047, `make cut-smoke`) |
| `x86_ahci_smoke.py` | the volume of `ahci0p1` on the SATA controller of a chipset: read-only by the rule of ADR 0041, the SHA-256 of the image before and after (`make ahci-smoke`) |
| `x86_nvme_smoke.py` | the same on the NVMe controller: the admin queue and one pair of I/O queues (`make nvme-smoke`, ADR 0042) |
| `x86_volume_smoke.py` | a volume of 5 GiB of ext2 and of MicroFS, read back by `e2fsck`, `debugfs` and the `fsck` of the machine (ADR 0029, `make x86-volume-smoke`) |
| `x86_smp_smoke.py` | two processors of the QEMU machine under `KERNEL_SMP` (`make x86-smp-smoke`, ADR 0012) |
| `x86_net_smoke.py` | the network of the x86-64 machine against servers of the host, including TLS and `pkg` over HTTPS |
| `x86_bench.py` | the numbers of the machine: `getpid`, fork and exec, a pipe, a file of 16 MiB and the console, on one boot of fixed sizes (`make bench`) |
| `io_bench.py` | the grain of the I/O: the size of a block and of the pipe behind a stage, measured on one boot |
| `ports_build.sh` | build the programs of the gate with the SDK alone, and nothing of this tree on the link line |
| `ports_smoke.py` | the gate of a program nobody rewrote: the console set of sbase and kilo out of `/bin/ports` (`make ports-smoke`) |
| `tty_smoke.py` | a full-screen editor typed at the machine: `kilo` saves with Ctrl+S and `cat` reads the line back (ADR 0051, `make tty-smoke`) |
| `uefi_esp.py` | the ESP of a firmware machine: one FAT volume with the loader, the kernel and `disk.img` (`make uefi`) |
| `uefi_smoke.py` | boot that volume under OVMF and check the loader, the kernel, the shell and the picture of the GOP frame (`make uefi-smoke`) |
| `uefi_stick.py` | the stick of a person: a GPT with the ESP, and the MicroFS volume behind it (`make uefi-stick`, ADR 0056) |
| `uefi_stick_smoke.py` | boot the stick as a USB disk under OVMF, write a file and find it in the second boot (`make uefi-stick-smoke`) |
| `x86_iso.py` | the bootable medium: the loader, the kernel and the root image in one ISO that boots in a BIOS and in a firmware |
| `x86_iso_smoke.py` | boot that ISO with `-cdrom` and check the shell and `/bin` (`make x86-iso-smoke`) |
| `x86_iso_uefi_smoke.py` | the same image from a CD and from a USB stick, on OVMF and on a BIOS (`make x86-iso-uefi-smoke`, `x86-iso-usb-smoke`) |
| `qemu_session.py` | one boot of the port under the control of a script: the read loop the smoke tests of the campaign share |
| `x86_image.py` | write the disk image with `/bin` and `/etc` (calls `microos_mkfs`) |
| `mkfs_mfs.c` | the host tool that formats and fills a MicroFS volume (`microos_mkfs`) |
| `mkfs_mfs_image.c` | build an image with a MicroFS volume, for the smokes that make their own |
| `progs_image.py` | the `progs` partition of the board: the `.mox` images collected into one (ADR 0018) |
| `check_image_fits.cmake` | the application image and its signature trailer fit an application slot, with the room a 1.x build needs to grow |
| `ota_usb.py` | update the board over USB into the inactive application slot, with the rollback (ADR 0022) |
| `magic_subset.py` | the compact database of libmagic the image of `file` carries |
| `mkprog.sh` | turn the ELF of a program into a MOX1 image |
| `link_program.sh` | link a program at the window of its port, check where the text landed and make the image |
| `bundle.sh` | collect `dist/`: the board images, the x86-64 tarball, the signature and the sums |
| `microos_sign.c` | sign, verify and inspect the trailer of an image (ADR 0020) |
| `integrity_check.py` | the repository checks: format, git, hygiene, bundle, version |
| `wiki_check.py` | every link and anchor of `docs/wiki` (`make wiki-check`) |
| `sram_budget.py`, `coverage_report.py` | the static budget and the coverage report |
| `spleen_font.py` | the 8x16 console font of x86-64 out of the BDF of Spleen |
| `test_mkprog.py` | the test of `mkprog.sh`: the header, the padding and the data of an image |
| `soak.py`, `host_stress.c`, `board_soak.sh` | the campaign of 1.0: the mixed load of the 600 seconds |
| `soak_host.py`, `board_soak_session.py` | the two halves of that session: the kernel over the host port, and one boot of the board |
| `soak_image.py`, `disk_stress.py` | the volume of the session filled to `ENOSPC`, and a disk that is filled, written, repaired and booted again |
| `pipe_smp_repro.py` | the pipeline of the smoke, five hundred times on two cores (the failure of the session of 1.0) |
| `_bench.py` | a write and a read of 64 MiB by `/bin/mkbig`, on ext2 or on MicroFS |

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
puts it at `0x12F000`, the last 4 KiB of `ota_0`, where `app_main` reads it
(an update writes the trailer at the end of `ota_1`, `0x24F000`, ADR 0022).
The board's check is electronic and its policy is the Kconfig
`MICRO_OS_REQUIRE_SIGNATURE` (ADR 0020, sections 6 and 7).
