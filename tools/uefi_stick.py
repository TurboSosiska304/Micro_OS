#!/usr/bin/env python3
"""Micro OS — the stick of a UEFI machine: an ESP and a volume of this system.

A person's laptop has a firmware of UEFI and a USB port, and no driver of the
machine is told about the disk: the firmware reads `\\EFI\\BOOT\\BOOTX64.EFI`
from a FAT partition, and the loader starts the kernel. That is enough for a
machine that only reads - and useless for a machine that writes, because the
volume of the boot was a file of the ESP copied to the memory (the loader,
step 1). A write to the memory is a write a power cut loses.

This tool writes the medium a person carries: one GPT with two partitions,

    partition 1  ESP, FAT32        EFI/BOOT/BOOTX64.EFI, microos64.bin and
                                   disk.img, the read-only machine of a boot
                                   that has no driver (the fallback)
    partition 2  MicroFS (ADR 0056) the volume of the machine, written by
                                   the kernel through the USB port

The kernel prefers the second partition when it finds it (a volume of this
system, named in /etc/volumes as `usb0p2`), so a file a person writes is a
sector of the stick and not a byte of the memory. The ESP keeps the image the
loader has always handed over: a machine whose USB controller is not brought
up still boots, read-only.

    tools/uefi_stick.py --image build-uefi/stick.img --build build-x86
                        --mkfs build-host/tools/microos_mkfs
                        [--esp-mb 64] [--volume-mb 24] [--second mfs|fat32]

`--second fat32` is the negative half of the gate: the second partition is a
volume of another system, the kernel does not take it for a root, and the
stick stays as it was.

The tools of the host write the parts: `tools/x86_image.py --volume-only` for
the volume (the same builder of the tree the disk uses), `tools/uefi_esp.py`
for the ESP, `sfdisk` for the table, and `dd` to put the two files in their
partitions. A machine without them is told so and the run stops with 2.

The exit status is 0 when the stick was written, 2 when a tool or a file is
missing, and 1 when one of them failed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

# The two partitions of the stick, in the sectors of 512 bytes of the table.
ESP_START_LBA = 2048
ESP_MB = 64
VOLUME_MB = 24
# The backup of the GPT at the end of the medium: 33 sectors and the copy of
# the first one, rounded up to a megabyte by the tool.
GPT_TAIL_SECTORS = 34

# The type GUIDs of the two partitions of the specification and of Linux.
ESP_TYPE = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
LINUX_TYPE = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

# The volume list of the stick: the four disks of the port and the partition
# of the stick itself, which is the name the kernel gives the second partition
# of `usb0`.
STICK_VOLUMES = ("ata0p1", "virtio0p1", "ahci0p1", "nvme0p1", "usb0p2")


def tool(name: str) -> str | None:
    return shutil.which(name)


def run(command: list, verbose: bool) -> int:
    if verbose:
        print("+ " + " ".join(command), file=sys.stderr)

    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    if result.returncode != 0:
        if verbose or result.stdout:
            print(result.stdout, file=sys.stderr)

    return result.returncode


def build_esp(image: str, build_dir: str, esp_mb: int, verbose: bool) -> int:
    """The FAT32 of the firmware: the loader, the kernel and the fallback image."""
    from uefi_esp import write as write_esp

    loader = os.path.join(build_dir, "BOOTX64.EFI")
    kernel = os.path.join(build_dir, "microos64.bin")
    disk = os.path.join(build_dir, "programs.img")

    for path in (loader, kernel, disk):
        if not os.path.isfile(path):
            print(f"missing {path}: build the machine first (make x86-build x86-disk)",
                  file=sys.stderr)
            return 2

    return write_esp(image, loader, kernel, disk, esp_mb, verbose)


def build_volume(image: str, build_dir: str, mkfs: str, volume_mb: int, second: str,
                 ports: str | None, verbose: bool) -> int:
    """The second partition: a MicroFS volume of this system, or a foreign one."""
    if not os.path.isfile(mkfs):
        print(f"missing {mkfs}: run make build", file=sys.stderr)
        return 2

    if second == "mfs":
        from x86_image import build_image

        return build_image(
            image,
            build_dir,
            mkfs,
            volume_mb,
            fs="mfs",
            data_mb=0,
            volumes=STICK_VOLUMES,
            journal=True,
            volume_only=True,
            ports=ports,
            verbose=verbose,
        )

    # The negative half: a volume of another system, made by the tools of the
    # host and not by the filesystem of this machine.
    with open(image, "wb") as file:
        file.truncate(volume_mb << 20)

    if run(["mkfs.fat", "-F", "32", "--invariant", "-n", "FOREIGN", image], verbose) != 0:
        print("mkfs.fat failed", file=sys.stderr)
        return 1

    return 0


def write_table(image: str, esp_sectors: int, volume_start: int, volume_sectors: int,
                verbose: bool) -> int:
    """The GPT: the ESP and the volume, in the order the loader finds them."""
    script = (
        "label: gpt\n"
        "unit: sectors\n"
        f"start={ESP_START_LBA}, size={esp_sectors}, type={ESP_TYPE}, "
        "name=\"ESP\", uuid=1B4E28BA-2FA1-11D2-883F-0016D3CCA427\n"
        f"start={volume_start}, size={volume_sectors}, type={LINUX_TYPE}, "
        "name=\"MICROOS\", uuid=6C7C0F3A-2F1A-4E5E-9E7D-6F2E1C9B0A11\n"
    )

    if verbose:
        print(script, file=sys.stderr)

    result = subprocess.run(["sfdisk", "-q", image], input=script, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print("sfdisk failed", file=sys.stderr)
        return 1

    return 0


def place(image: str, part: str, sector: int, verbose: bool) -> int:
    """Copy one file of the host into its partition of the stick."""
    if verbose:
        print(f"+ dd if={part} of={image} bs=512 seek={sector}", file=sys.stderr)

    result = subprocess.run(
        ["dd", f"if={part}", f"of={image}", "bs=512", f"seek={sector}", "conv=notrunc", "status=none"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        return 1

    return 0


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="write the stick of a UEFI machine")
    parser.add_argument("--image", default=os.path.join(ROOT, "build-uefi", "stick.img"))
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--esp-mb", type=int, default=ESP_MB, help="the size of the ESP (64)")
    parser.add_argument("--volume-mb", type=int, default=VOLUME_MB,
                        help="the size of the volume (24)")
    parser.add_argument("--second", choices=("mfs", "fat32"), default="mfs",
                        help="the volume of the second partition (default: mfs)")
    parser.add_argument("--ports", default=None, metavar="DIR",
                        help="the programs of the SDK into /bin/ports of the volume "
                        "(default: none; `make uefi-stick` passes build-ports/programs)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    for name in ("sfdisk", "dd"):
        if tool(name) is None:
            print(f"missing {name}", file=sys.stderr)
            return 2

    work = os.path.join(os.path.dirname(os.path.abspath(args.image)), "parts")
    os.makedirs(work, exist_ok=True)
    esp = os.path.join(work, "esp.img")
    volume = os.path.join(work, "volume.img")

    if build_esp(esp, args.build, args.esp_mb, args.verbose) != 0:
        print("the ESP of the stick is not written", file=sys.stderr)
        return 1

    if build_volume(volume, args.build, args.mkfs, args.volume_mb, args.second, args.ports,
                    args.verbose) != 0:
        print("the volume of the stick is not written", file=sys.stderr)
        return 1

    esp_bytes = os.path.getsize(esp)
    volume_bytes = os.path.getsize(volume)

    if esp_bytes % 512 or volume_bytes % 512:
        print("a part is not a whole number of sectors", file=sys.stderr)
        return 1

    esp_sectors = esp_bytes // 512
    volume_start = ESP_START_LBA + esp_sectors
    volume_sectors = volume_bytes // 512
    total_sectors = volume_start + volume_sectors + GPT_TAIL_SECTORS
    total_bytes = ((total_sectors + 2047) // 2048) * 2048 * 512

    with open(args.image, "wb") as file:
        file.truncate(total_bytes)

    if write_table(args.image, esp_sectors, volume_start, volume_sectors, args.verbose) != 0:
        return 1

    if place(args.image, esp, ESP_START_LBA, args.verbose) != 0:
        return 1

    if place(args.image, volume, volume_start, args.verbose) != 0:
        return 1

    print(f"stick: {args.image}, {total_bytes // (1 << 20)} MB, "
          f"ESP {esp_sectors * 512 // (1 << 20)} MB at LBA {ESP_START_LBA}, "
          f"{args.second} {volume_sectors * 512 // (1 << 20)} MB at LBA {volume_start}")
    print(f"  -drive file={args.image},format=raw,if=none,id=stick "
          f"-device qemu-xhci -device usb-storage,drive=stick,bus=xhci.0")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
