#!/usr/bin/env python3
"""Micro OS — the bootable ISO of the x86-64 port.

The kernel of this port needs a boot loader: it is a multiboot 1 image that
copies the 64-bit kernel and turns the long mode on, and a machine that starts
from a CD has to find that loader in the El Torito catalogue of the ISO first.
This tool stages the three files of the boot and asks the boot loader of the
machine to write the image:

    /boot/loader32.elf   the loader of the port (module 0 is the kernel)
    /boot/microos64.bin  the signed kernel
    /boot/root.img       the root image: a MicroFS disk with /bin and /etc
    /boot/grub/grub.cfg  or /boot/limine.conf, the menu of the boot

    tools/x86_iso.py [--loader PATH] [--kernel PATH] [--root PATH]
                     [--out PATH] [--stage DIR] [--bootloader auto|grub|limine]
                     [--volume-label LABEL] [--keep] [--verbose]

`--bootloader auto` prefers grub-mkrescue, the boot loader the ADR names, and
falls back to limine when the system has none. The two write the same boot:
the loader, the kernel and the root image are the three files above, and the
menu of either asks for `multiboot` of the first and one `module` for each of
the other two. GRUB is not vendored: the build uses the copy of the system, or
says which package to install.

The exit status is 0 when the ISO was written, 2 when the boot loader or one
of its files is missing, and 1 when a tool failed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86-iso")

# Where limine keeps its BIOS stage on this machine. The distribution puts it
# in /usr/share/limine (Arch, CachyOS) or /usr/lib/limine (Debian, Fedora);
# LIMINE_DIR names another place without a flag of the command.
LIMINE_DIRS = ["/usr/share/limine", "/usr/lib/limine", "/usr/local/share/limine"]

# The three files of the boot, in the order the loader expects them. Module 0
# is the kernel (the loader copies it to 16 MiB); module 1 is the root image
# (the port mounts its first partition, ADR 0026). Nothing else is loaded.
GRUB_CONFIG = """\
# Micro OS — the x86-64 machine from its CD (ADR 0026).
#
# The loader is a multiboot 1 image: it copies the kernel to 16 MiB, builds
# the map of the first gigabyte and the kernel at the top of the address
# space, and enters the 64-bit kernel. The root image is the second module;
# the loader passes the information of the boot through, and the port mounts
# the first partition of the module at / (read-only: the medium is a CD).
set timeout=0
set default=0

menuentry "Micro OS (x86-64, BIOS)" {
    multiboot /boot/loader32.elf
    module /boot/microos64.bin
    module /boot/root.img
    boot
}
"""

LIMINE_CONFIG = """\
# Micro OS — the x86-64 machine from its CD (ADR 0026).
#
# The loader is a multiboot 1 image: it copies the kernel to 16 MiB, builds
# the map of the first gigabyte and the kernel at the top of the address
# space, and enters the 64-bit kernel. The root image is the second module;
# the port mounts the first partition of the module at / (read-only: the
# medium is a CD). The paths are from the root of the ISO, and `boot()` is
# the volume the configuration was read from.

timeout: 0
default_entry: 1

/Micro OS (x86-64, BIOS)
    protocol: multiboot1
    path: boot():/boot/loader32.elf
    module_path: boot():/boot/microos64.bin
    module_path: boot():/boot/root.img
"""


def which(name: str) -> str | None:
    return shutil.which(name)


def find_limine_dir() -> str | None:
    """The directory with limine-bios.sys and limine-bios-cd.bin."""
    override = os.environ.get("LIMINE_DIR")
    candidates = [override] if override else []
    candidates += LIMINE_DIRS

    for path in candidates:
        if not path:
            continue
        if os.path.isfile(os.path.join(path, "limine-bios.sys")) and os.path.isfile(
            os.path.join(path, "limine-bios-cd.bin")
        ):
            return path

    return None


def copy_file(source: str, target: str) -> None:
    if not os.path.isfile(source):
        raise FileNotFoundError(f"missing {source}")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(source, target)


def stage_tree(stage: str, loader: str, kernel: str, root: str, bootloader: str) -> None:
    """Write the tree of the ISO, with the menu of the boot loader it gets."""
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


def build_limine(stage: str, out: str, label: str, verbose: bool) -> int:
    limine_dir = find_limine_dir()

    if limine_dir is None:
        print("limine is not installed: /usr/share/limine/limine-bios.sys is missing",
              file=sys.stderr)
        print("(Arch: sudo pacman -S limine; Debian: sudo apt install limine)", file=sys.stderr)
        return 2

    for name in ("limine-bios.sys", "limine-bios-cd.bin"):
        copy_file(os.path.join(limine_dir, name), os.path.join(stage, "boot", name))

    # The same inputs, the same bytes: the moment of the volume and of every
    # file comes from SOURCE_DATE_EPOCH (make exports it), in the format of
    # xorriso - YYYYMMDDhhmmsscc, the century being the last two digits.
    # Without this the image of two builds of one tree differs in the time
    # stamps of the volume descriptor and of the files.
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0") or "0")
    stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime(epoch)) + "00"

    # The catalogue of El Torito names the CD boot record of limine; the
    # record loads limine-bios.sys from the ISO. No `limine bios-install`:
    # that one writes the boot sector of a disk, and this is a CD.
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
        "-R",
        "-J",
        stage,
    ]

    if verbose:
        print(" ".join(command))

    # No `limine bios-install`: that command patches the boot sector of a
    # disk, and this image boots through the catalogue of El Torito, where
    # the record of `limine-bios-cd.bin` reads `limine-bios.sys` from the ISO
    # itself. A hybrid image that also boots from a USB stick is a debt of
    # ADR 0026, not a step of this build.
    return subprocess.run(command, check=False).returncode


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


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the bootable ISO of the x86-64 port")
    parser.add_argument("--loader", default=os.path.join(DEFAULT_BUILD, "loader32.elf"))
    parser.add_argument("--kernel", default=os.path.join(DEFAULT_BUILD, "microos64.bin"))
    parser.add_argument("--root", default=os.path.join(DEFAULT_BUILD, "root.img"))
    parser.add_argument("--out", default=os.path.join(DEFAULT_BUILD, "microos.iso"))
    parser.add_argument("--stage", default=os.path.join(DEFAULT_BUILD, "iso"))
    parser.add_argument("--bootloader", choices=["auto", "grub", "limine"], default="auto")
    parser.add_argument("--volume-label", default="MICROOS")
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
            print("neither grub-mkrescue nor limine is installed; the ISO needs one of them",
                  file=sys.stderr)
        return 2

    for path in (args.loader, args.kernel, args.root):
        if not os.path.isfile(path):
            print(f"missing {path}: build the port and the root image first", file=sys.stderr)
            return 2

    if args.verbose:
        print(f"the boot loader of the ISO is {bootloader}")

    stage_tree(args.stage, args.loader, args.kernel, args.root, bootloader)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if os.path.exists(args.out):
        os.unlink(args.out)

    if bootloader == "grub":
        result = build_grub(args.stage, args.out, args.volume_label, args.verbose)
    else:
        result = build_limine(args.stage, args.out, args.volume_label, args.verbose)

    if result != 0:
        return result

    if not args.keep:
        shutil.rmtree(args.stage, ignore_errors=True)

    print(f"{args.out} ({os.path.getsize(args.out)} bytes, {bootloader})")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
