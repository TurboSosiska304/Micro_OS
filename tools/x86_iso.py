#!/usr/bin/env python3
"""Micro OS — the bootable medium of the x86-64 port: one image of four boots.

The kernel of this port needs a boot loader: it is a multiboot 1 image that
copies the 64-bit kernel and turns the long mode on. A machine that starts
from a CD finds the loader in the El Torito catalogue of the image; a machine
that starts from a USB stick found it in the boot sector of its disk; a
firmware of UEFI finds it in a FAT partition. This tool writes an image with
all three of those entries, so that one file boots:

    a BIOS from its CD drive        the El Torito entry of limine-bios-cd.bin
    a BIOS from a USB stick         the MBR of `limine bios-install`
    a UEFI from its CD drive        the El Torito entry of limine-uefi-cd.bin
    a UEFI from a USB stick         the ESP of the GPT, the same FAT image

The tree of the image is the boot itself:

    /boot/loader32.elf   the loader of the port (module 0 is the kernel)
    /boot/microos64.bin  the signed kernel
    /boot/root.img       the root image: a MicroFS disk with /bin and /etc
    /boot/limine.conf    the menu of the boot
    /EFI/BOOT/BOOTX64.EFI  a copy of the EFI application, for the tools that
                         build a stick out of the tree of the ISO

    tools/x86_iso.py [--loader PATH] [--kernel PATH] [--root PATH]
                     [--out PATH] [--stage DIR] [--bootloader auto|grub|limine]
                     [--volume-label LABEL] [--no-hybrid] [--keep] [--verbose]

`--bootloader auto` prefers grub-mkrescue, the boot loader the ADR names, and
falls back to limine when the system has none. The two write the same boot:
the loader, the kernel and the root image are the three files above, and the
menu of either asks for `multiboot` of the first and one `module` for each of
the other two. GRUB is not vendored: the build uses the copy of the system, or
says which package to install.

The exit status is 0 when the image was written, 2 when the boot loader or one
of its files is missing, and 1 when a tool failed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86-iso")

# Where limine keeps its boot files on this machine. The distribution puts them
# in /usr/share/limine (Arch, CachyOS) or /usr/lib/limine (Debian, Fedora);
# LIMINE_DIR names another place without a flag of the command.
LIMINE_DIRS = ["/usr/share/limine", "/usr/lib/limine", "/usr/local/share/limine"]

# The files of limine this tool stages. `limine-bios.sys` is the second stage of
# the BIOS boot (read from the image by the record of the catalogue or by the
# MBR), `limine-bios-cd.bin` is the El Torito record of the BIOS, and
# `limine-uefi-cd.bin` is a FAT image holding EFI/BOOT/BOOTX64.EFI, which is at
# once the EFI record of the catalogue and the content of the ESP of the GPT.
LIMINE_FILES = ("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin")

# The EFI application itself, staged in the tree of the image: a tool that
# writes a stick by copying the tree (rather than by `dd` of the image) puts
# this file into the ESP it makes.
LIMINE_EFI = "BOOTX64.EFI"

# The three files of the boot, in the order the loader expects them. Module 0
# is the kernel (the loader copies it to 16 MiB); module 1 is the root image
# (the port mounts its first partition, ADR 0026). Nothing else is loaded.
GRUB_CONFIG = """\
# Micro OS — the x86-64 machine from its medium (ADR 0026).
#
# The loader is a multiboot 1 image: it copies the kernel to 16 MiB, builds
# the map of the first gigabyte and the kernel at the top of the address
# space, and enters the 64-bit kernel. The root image is the second module;
# the loader passes the information of the boot through, and the port mounts
# the first partition of the module at / (read-only: the medium is a CD or a
# stick written with the image, and neither is a place to write).
set timeout=0
set default=0

menuentry "Micro OS (x86-64)" {
    multiboot /boot/loader32.elf
    module /boot/microos64.bin
    module /boot/root.img
    boot
}
"""

LIMINE_CONFIG = """\
# Micro OS — the x86-64 machine from its medium (ADR 0026).
#
# The loader is a multiboot 1 image: it copies the kernel to 16 MiB, builds
# the map of the first gigabyte and the kernel at the top of the address
# space, and enters the 64-bit kernel. The root image is the second module;
# the port mounts the first partition of the module at / (read-only: the
# medium is a CD or a stick written with the image). The paths are from the
# root of the image, and `boot()` is the volume the configuration was read
# from — the ISO9660 of the image, on a CD and on a stick alike.

timeout: 0
default_entry: 1

/Micro OS (x86-64)
    protocol: multiboot1
    path: boot():/boot/loader32.elf
    module_path: boot():/boot/microos64.bin
    module_path: boot():/boot/root.img
"""


def which(name: str) -> str | None:
    return shutil.which(name)


def find_limine_dir() -> str | None:
    """The directory with limine-bios.sys and the two boot images."""
    override = os.environ.get("LIMINE_DIR")
    candidates = [override] if override else []
    candidates += LIMINE_DIRS

    for path in candidates:
        if not path:
            continue
        if all(os.path.isfile(os.path.join(path, name)) for name in LIMINE_FILES):
            return path

    return None


def copy_file(source: str, target: str) -> None:
    if not os.path.isfile(source):
        raise FileNotFoundError(f"missing {source}")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(source, target)


def stage_tree(stage: str, loader: str, kernel: str, root: str, bootloader: str,
               limine_dir: str | None) -> None:
    """Write the tree of the image, with the menu of the boot loader it gets."""
    if os.path.isdir(stage):
        shutil.rmtree(stage)

    copy_file(loader, os.path.join(stage, "boot", "loader32.elf"))
    copy_file(kernel, os.path.join(stage, "boot", "microos64.bin"))
    copy_file(root, os.path.join(stage, "boot", "root.img"))

    if bootloader == "grub":
        config = os.path.join(stage, "boot", "grub", "grub.cfg")
        os.makedirs(os.path.dirname(config), exist_ok=True)
        with open(config, "w", encoding="utf-8") as file:
            file.write(GRUB_CONFIG)
        return

    config = os.path.join(stage, "boot", "limine.conf")
    with open(config, "w", encoding="utf-8") as file:
        file.write(LIMINE_CONFIG)

    # The copy of the EFI application in the tree is not the ESP (that one is
    # the FAT image of the El Torito record and of the GPT partition): it is
    # there for the tools that write a stick by copying the tree, and because
    # xorriso warns about a medium with a UEFI entry and no /EFI/BOOT.
    assert limine_dir is not None
    copy_file(os.path.join(limine_dir, LIMINE_EFI),
              os.path.join(stage, "EFI", "BOOT", LIMINE_EFI))


def build_grub(stage: str, out: str, label: str, verbose: bool) -> int:
    command = [
        "grub-mkrescue",
        "--output",
        out,
        "--",
        "-volid",
        label,
        stage,
    ]

    if verbose:
        print(" ".join(command))

    return subprocess.run(command, check=False).returncode


def disk_guid(label: str) -> str:
    """The identity of the disk of the image, from its label.

    xorriso would take a random one (and a random signature of the MBR behind
    it), and two builds of one tree would then be two different disks. The
    name of the volume is the seed: the same label, the same disk.
    """
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"microos-iso:{label}"))


def stamp_of_the_build() -> str:
    """The moment of the volume, from SOURCE_DATE_EPOCH, for xorriso.

    The same inputs, the same bytes: the moment of the volume and of every
    file comes from SOURCE_DATE_EPOCH (make exports it), in the format of
    xorriso - YYYYMMDDhhmmsscc, the century being the last two digits.
    Without this the image of two builds of one tree differs in the time
    stamps of the volume descriptor and of the files.
    """
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0") or "0")
    return time.strftime("%Y%m%d%H%M%S", time.gmtime(epoch)) + "00"


def build_limine(stage: str, out: str, label: str, hybrid: bool, verbose: bool) -> int:
    limine_dir = find_limine_dir()
    limine_tool = which("limine")

    if limine_dir is None:
        print("limine is not installed: /usr/share/limine/limine-bios.sys is missing",
              file=sys.stderr)
        print("(Arch: sudo pacman -S limine; Debian: sudo apt install limine)", file=sys.stderr)
        return 2

    if hybrid and limine_tool is None:
        print("the command `limine` is not on PATH: it installs the boot sector of the image",
              file=sys.stderr)
        print("(Arch: sudo pacman -S limine; Debian: sudo apt install limine)", file=sys.stderr)
        return 2

    for name in LIMINE_FILES:
        copy_file(os.path.join(limine_dir, name), os.path.join(stage, "boot", name))

    stamp = stamp_of_the_build()

    # The catalogue of El Torito names two boot records: the BIOS one, which
    # loads limine-bios.sys from the image, and the EFI one, whose extent is a
    # FAT image with EFI/BOOT/BOOTX64.EFI in it. `--efi-boot-part` makes that
    # extent a partition of the GPT (`--efi-boot-image` gives it the right
    # entry of the catalogue), and `--protective-msdos-label` writes the
    # protective MBR the GPT needs. `-isohybrid-gpt-basdat` marks the El
    # Torito image as a basic data partition as well, which is what a part of
    # the tools of the world look for.
    command = [
        which("xorriso") or "xorriso",
        "-as",
        "mkisofs",
        "-o",
        out,
        "-V",
        label,
        f"--modification-date={stamp}",
        "--set_all_file_dates",
        stamp,
        "-b",
        "boot/limine-bios-cd.bin",
        "-no-emul-boot",
        "-boot-load-size",
        "4",
        "-boot-info-table",
    ]

    if hybrid:
        command += [
            # The EFI record is the alternative of the catalogue; --efi-boot
            # adds that record itself (`-eltorito-alt-boot -e ... -no-emul-boot`
            # in the vocabulary of the command).
            "-apm-block-size",
            "2048",
            "--efi-boot",
            "boot/limine-uefi-cd.bin",
            "-efi-boot-part",
            "--efi-boot-image",
            "--protective-msdos-label",
            "-isohybrid-gpt-basdat",
            # The disk of the image has an identity, and it is the label: a
            # random GUID would make two builds of one tree two different
            # disks (and the MBR behind the GPT carries its own copy).
            "--gpt_disk_guid",
            disk_guid(label),
        ]
    # Without --hybrid the image is a CD of one record: the BIOS one. It boots
    # from a CD drive and from nothing else.

    command += ["-R", "-J", stage]

    if verbose:
        print(" ".join(command))

    result = subprocess.run(command, check=False).returncode

    if result != 0:
        return result

    if not hybrid:
        return 0

    # A CD boots through the catalogue and needs nothing else. A stick boots
    # through the boot sector of its disk: `limine bios-install` writes the
    # first stage there and the second stage in the system area of the image
    # (32 KiB before the ISO9660 descriptor, which limine leaves alone), and
    # keeps the partition table it finds — the protective MBR and the GPT with
    # the ESP stay where xorriso wrote them. The BIOS of a stick and the
    # firmware of a UEFI, from one file.
    if verbose:
        print(f"{limine_tool} bios-install {out}")

    result = subprocess.run([limine_tool, "bios-install", out], check=False).returncode

    if result != 0:
        print("limine bios-install failed: the image boots from a CD, not from a stick",
              file=sys.stderr)
        return result

    return 0


def choose_bootloader(requested: str) -> str | None:
    if requested == "grub":
        return "grub" if which("grub-mkrescue") else None
    if requested == "limine":
        return "limine" if find_limine_dir() else None

    if which("grub-mkrescue"):
        return "grub"
    if find_limine_dir():
        return "limine"

    return None


def report(out: str, bootloader: str, hybrid: bool) -> None:
    """Say what the image is: the size and the four boots it carries."""
    size = os.path.getsize(out)
    kind = "CD and stick" if hybrid else "CD only"
    print(f"{out} ({size} bytes, {bootloader}, {kind})")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the bootable medium of the x86-64 port")
    parser.add_argument("--loader", default=os.path.join(DEFAULT_BUILD, "loader32.elf"))
    parser.add_argument("--kernel", default=os.path.join(DEFAULT_BUILD, "microos64.bin"))
    parser.add_argument("--root", default=os.path.join(DEFAULT_BUILD, "root.img"))
    parser.add_argument("--out", default=os.path.join(DEFAULT_BUILD, "microos.iso"))
    parser.add_argument("--stage", default=os.path.join(DEFAULT_BUILD, "iso"))
    parser.add_argument("--bootloader", choices=["auto", "grub", "limine"], default="auto")
    parser.add_argument("--volume-label", default="MICROOS")
    parser.add_argument("--no-hybrid", action="store_true",
                        help="a CD only: no EFI record, no GPT and no boot sector of a disk")
    parser.add_argument("--keep", action="store_true", help="keep the staged tree")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    bootloader = choose_bootloader(args.bootloader)

    if bootloader is None:
        if args.bootloader == "grub":
            print("grub-mkrescue is not installed", file=sys.stderr)
            print("(Arch: sudo pacman -S grub; Debian: sudo apt install grub-pc-bin grub-common)",
                  file=sys.stderr)
        elif args.bootloader == "limine":
            print("limine is not installed: limine-bios.sys is missing", file=sys.stderr)
        else:
            print("neither grub-mkrescue nor limine is installed; the image needs one of them",
                  file=sys.stderr)
        return 2

    for path in (args.loader, args.kernel, args.root):
        if not os.path.isfile(path):
            print(f"missing {path}: build the port and the root image first", file=sys.stderr)
            return 2

    # GRUB writes a hybrid image of its own (its `boot_hybrid.img` is the boot
    # sector of a stick and its EFI record comes from grub-efi), so the flag of
    # the hybrid belongs to the limine path alone.
    hybrid = not args.no_hybrid and bootloader == "limine"

    if args.verbose:
        print(f"the boot loader of the image is {bootloader}")
        if args.no_hybrid:
            print("the image is a CD only: it does not boot from a stick")

    limine_dir = find_limine_dir() if bootloader == "limine" else None
    stage_tree(args.stage, args.loader, args.kernel, args.root, bootloader, limine_dir)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if os.path.exists(args.out):
        os.unlink(args.out)

    if bootloader == "grub":
        result = build_grub(args.stage, args.out, args.volume_label, args.verbose)
    else:
        result = build_limine(args.stage, args.out, args.volume_label, hybrid, args.verbose)

    if result != 0:
        return result

    if not args.keep:
        shutil.rmtree(args.stage, ignore_errors=True)

    report(args.out, bootloader, hybrid)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
