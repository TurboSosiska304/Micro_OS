#!/usr/bin/env python3
"""Micro OS — the gate of an install on a separate disk (L3 and L4).

The other smokes boot the disk they check. An install must not touch the disk
the machine booted from, so this tool boots a live image on the IDE bus and
attaches a *different*, 32 MiB raw image as nvme0. On that image it runs

    install --disk nvme0 --size 24M --fs ext2 --confirm "INSTALL nvme0 24M"

and the same with --fs mfs. Besides the L3 checks it reads the manifest of
the copy, repeats the check with --verify, corrupts one byte of a file on the
medium and requires the next --verify to name that file and refuse. Nothing
here is wired into CI yet: it is a gate a person runs before a real disk.

MicroFS holds one volume in one static slot. The live root is therefore an
ext2 image for the mfs case and the ordinary mfs image for the ext2 case, so
the volume under test is never the root it displaced.

    python3 tools/x86_install_smoke.py [--build DIR] [--mkfs PATH]
                                       [--dir DIR] [--timeout 180] [--keep] [--verbose]

Exit status: 0 every check passed, 2 a tool or an image is missing, 1 a check
failed. At most one QEMU runs at a time; an orphan older than twenty minutes
whose parent is gone is reaped before the run.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from qemu_session import Machine, MachineError, MachineTimeout
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

DISK_MB = 32
DISK_BYTES = DISK_MB * 1024 * 1024
SECTOR = 512
SIZE_TEXT = "24M"
SIZE_SECTORS = 24 * 1024 * 1024 // SECTOR
ESP_START = 2048
ESP_SIZE = 2048
GPT_ARRAY_LBA = 2
GPT_ENTRY_SIZE = 128
ORPHAN_AGE_S = 20 * 60

ESP_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

CHECKS: list = []


def check(title: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((title, bool(ok), detail))


def reap_orphans() -> None:
    """Take away a QEMU whose parent died and which has been alone too long."""
    try:
        ticks = os.sysconf("SC_CLK_TCK")
        uptime = float(open("/proc/uptime", encoding="ascii").read().split()[0])
    except (OSError, ValueError):
        return

    boot = time.time() - uptime
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue

        try:
            raw = open(f"/proc/{name}/stat", encoding="utf-8").read()
            cmd = open(f"/proc/{name}/cmdline", "rb").read()
        except OSError:
            continue

        if b"qemu-system-x86_64" not in cmd:
            continue

        fields = raw.rsplit(")", 1)[-1].split()
        if len(fields) < 20:
            continue

        ppid = int(fields[1])
        start = boot + int(fields[19]) / ticks

        if ppid > 1 and os.path.exists(f"/proc/{ppid}"):
            continue
        if time.time() - start < ORPHAN_AGE_S:
            continue

        try:
            os.kill(int(name), signal.SIGTERM)
        except OSError:
            pass


def make_target(path: str) -> None:
    """A blank GPT disk with one ESP and a gap the install can use."""
    with open(path, "wb") as handle:
        handle.truncate(DISK_BYTES)

    script = (
        "label: gpt\n"
        f"start={ESP_START}, size={ESP_SIZE}, "
        f'type={ESP_GUID}, name="EFI System"\n'
    )
    result = subprocess.run(
        ["sfdisk", "-q", path], input=script.encode(), capture_output=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())


def gpt_entry(image: str, index: int) -> bytes:
    with open(image, "rb") as handle:
        handle.seek(GPT_ARRAY_LBA * SECTOR + index * GPT_ENTRY_SIZE)
        return handle.read(GPT_ENTRY_SIZE)


def disk_hash(image: str) -> str:
    digest = hashlib.sha256()
    with open(image, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)

    return digest.hexdigest()


def extract_partition(image: str, out: str) -> None:
    """The new volume alone, so the host's e2fsck can read it."""
    with open(image, "rb") as source, open(out, "wb") as volume:
        source.seek((ESP_START + ESP_SIZE) * SECTOR)
        volume.write(source.read(SIZE_SECTORS * SECTOR))


def ensure_live(build: str, mkfs: str, fs: str, directory: str, marker: bytes) -> str:
    """The live root of a case: ext2 leaves the MicroFS slot for the target.

    One extra file of known bytes is part of the tree, so the corruption of
    the copy can be made on the host at a byte the host can find without
    knowing the layout of the filesystem the installer wrote.
    """
    path = os.path.join(directory, f"install-live-{fs}-{marker[:8].decode()}.img")
    if os.path.isfile(path):
        return path

    extra = ["--text", f"/etc/corrupt-me={marker.decode()}"]
    build_image(path, build_dir=build, mkfs=mkfs, fs=fs, extra=extra)
    return path


def corrupt_marker(image: str, marker: bytes) -> int:
    """Change one byte of the copied file, in place, on the target image."""
    with open(image, "rb") as handle:
        data = handle.read()

    at = data.find(marker)
    if at < 0 or data.count(marker) != 1:
        raise RuntimeError(f"the marker is not once on {image}")

    with open(image, "r+b") as handle:
        handle.seek(at)
        old = handle.read(1)
        handle.seek(at)
        handle.write(b"X" if old != b"X" else b"Y")

    return at


def run_case(build: str, live: str, target: str, fs: str, marker: bytes, timeout: float,
             verbose: bool) -> None:
    make_target(target)
    esp_before = gpt_entry(target, 0)
    before = disk_hash(target)

    extra = [
        "-drive", f"file={target},format=raw,if=none,id=instnvme,cache=writeback",
        "-device",
        "nvme,serial=install,drive=instnvme,logical_block_size=1024,physical_block_size=1024",
    ]

    answers = {}
    after_dry = None
    commands = [
        ("dry", f"install --disk nvme0 --size {SIZE_TEXT} --fs {fs}"),
        ("install",
         f'install --disk nvme0 --size {SIZE_TEXT} --fs {fs} '
         f'--confirm "INSTALL nvme0 {SIZE_TEXT}"'),
        ("blockdev", "cat /proc/blockdev"),
        ("mount", f"mount nvme0p2 /mnt/target {fs}"),
        ("ls", "ls /mnt/target"),
        ("bin", "ls /mnt/target/bin"),
        ("boot", "ls /mnt/target/boot"),
        ("etc", "ls /mnt/target/etc"),
        ("record", "cat /mnt/target/etc/install"),
        ("volumes", "cat /mnt/target/etc/volumes"),
        ("manifest", "cat /mnt/target/etc/install.manifest"),
        ("manifesthash", "cat /mnt/target/etc/install.manifest.sha256"),
        ("umount", "umount /mnt/target"),
        ("verify", f"install --disk nvme0 --fs {fs} --verify"),
    ]

    with Machine(build, live, disk="ide", extra_args=extra) as machine:
        try:
            machine.open(timeout=timeout)
            for key, command in commands:
                answers[key] = machine.run(command, timeout=timeout)
                if key == "dry":
                    after_dry = disk_hash(target)
        except (MachineError, MachineTimeout) as error:
            for key, _ in commands:
                answers.setdefault(key, "")
            check(f"{fs}: the machine answers the shell", False, str(error))
        if verbose:
            print(machine.transcript.decode("utf-8", "replace"))

    # The image after the install, before any read-only check of it.
    installed_hash = disk_hash(target)

    check(f"{fs}: a dry run writes nothing", after_dry == before,
          "the image changed before the confirmation")
    check(f"{fs}: the plan is a dry run", "dry-run, nothing written" in answers["dry"],
          answers["dry"])
    check(f"{fs}: the confirmed install completes and verifies",
          "installed, verified" in answers["install"], answers["install"])
    check(f"{fs}: the GPT entry was appended",
          'GPT entry "MICROOS_ROOT" appended' in answers["install"], answers["install"])
    check(f"{fs}: no error after the table write",
          "is still on nvme0" not in answers["install"], answers["install"])
    check(f"{fs}: the new partition is in /proc/blockdev",
          # /proc/blockdev counts the 1024-byte sectors of this NVMe, while
          # the GPT counts 512-byte ones: 24 MiB is 49152 LBA or 24576 blocks.
          re.search(rf"nvme0p2\s+\d+\s+{SIZE_SECTORS // 2}\b", answers["blockdev"]) is not None,
          answers["blockdev"])
    check(f"{fs}: the volume mounts",
          "is mounted at /mnt/target" in answers["mount"], answers["mount"])

    listing = answers["ls"]
    for name in ("dev", "etc", "mnt", "proc", "tmp", "boot", "bin"):
        check(f"{fs}: /{name} is on the volume", re.search(rf"\b{name}\b", listing) is not None,
              listing)

    bin_list = answers["bin"]
    for name in ("hello", "cat", "sqlite3"):
        check(f"{fs}: /bin/{name} was copied", re.search(rf"\b{name}\b", bin_list) is not None,
              bin_list)
    check(f"{fs}: the kernel module was copied",
          re.search(r"\bkernel\b", answers["boot"]) is not None, answers["boot"])

    etc = answers["etc"]
    for name in ("install", "volumes", "install.manifest", "install.manifest.sha256",
                 "corrupt-me"):
        check(f"{fs}: /etc/{name} is on the volume", name in etc, etc)

    record = answers["record"]
    for field in ("version: ", "source: fnv1a64:", "uuid: ", "date: ", "boot: uefi",
                  f"fs: {fs}", "device: nvme0p2"):
        check(f"{fs}: the install record names {field.strip()}", field in record, record)

    check(f"{fs}: /etc/volumes names the partition",
          re.search(r"\bnvme0p2\b", answers["volumes"]) is not None, answers["volumes"])

    manifest = answers["manifest"]
    for path in ("/boot/kernel", "/bin/hello", "/etc/install", "/etc/volumes",
                 "/etc/corrupt-me"):
        # The console of the port ends a line with CR CR LF, so the digest is
        # matched on its own rather than against an anchored end of line.
        pattern = rf"{re.escape(path)} \d+ [0-9a-f]{{64}}"
        check(f"{fs}: the manifest hashes {path}", re.search(pattern, manifest) is not None,
              manifest)
    check(f"{fs}: the manifest has a version line",
          "# Micro OS install manifest v1" in manifest, manifest)

    printed = re.search(r"manifest sha256:([0-9a-f]{64})", answers["install"])
    stored = re.search(r"([0-9a-f]{64})", answers["manifesthash"])
    check(f"{fs}: the console prints the manifest hash", printed is not None, answers["install"])
    check(f"{fs}: the hash on the volume is the one printed",
          printed is not None and stored is not None and printed.group(1) == stored.group(1),
          answers["manifesthash"])
    check(f"{fs}: every copied file verified",
          re.search(r"verified \d+ file\(s\), manifest sha256:", answers["install"]) is not None,
          answers["install"])
    check(f"{fs}: a clean --verify passes",
          "installed, verified" in answers["verify"], answers["verify"])

    checked = disk_hash(target)
    check(f"{fs}: verification writes nothing", checked == installed_hash,
          "the target image changed in a read-only pass")

    check(f"{fs}: the ESP entry is byte-identical", gpt_entry(target, 0) == esp_before)
    check(f"{fs}: the volume was unmounted", "is unmounted" in answers["umount"],
          answers["umount"])

    if fs == "ext2" and shutil.which("e2fsck") is not None:
        with tempfile.TemporaryDirectory(prefix="microos-install-") as work:
            volume = os.path.join(work, "root.ext2")
            extract_partition(target, volume)
            result = subprocess.run(
                ["e2fsck", "-fn", volume], capture_output=True, check=False
            )
            text = (result.stdout + result.stderr).decode("utf-8", "replace")
            check("ext2: e2fsck of the host finds no error",
                  "Filesystem still has errors" not in text and "Fix?" not in text, text)

    # One byte of a copied file is changed on the medium behind the kernel.
    # A second, read-only boot has to name that file and refuse the install.
    at = corrupt_marker(target, marker)
    damaged = disk_hash(target)
    check(f"{fs}: a byte of the copied file was changed", damaged != checked,
          f"byte {at} of {target}")

    corrupted = ""
    with Machine(build, live, disk="ide", extra_args=extra) as machine:
        try:
            machine.open(timeout=timeout)
            corrupted = machine.run(f"install --disk nvme0 --fs {fs} --verify", timeout=timeout)
        except (MachineError, MachineTimeout) as error:
            check(f"{fs}: the machine answers after corruption", False, str(error))
        if verbose:
            print(machine.transcript.decode("utf-8", "replace"))

    check(f"{fs}: a damaged copy fails the check",
          "MISMATCH /etc/corrupt-me" in corrupted and "installed, verified" not in corrupted,
          corrupted)
    check(f"{fs}: the failed check writes nothing", disk_hash(target) == damaged,
          "the target image changed in a failed read-only check")


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the x86 build directory")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host MicroFS tool")
    parser.add_argument("--dir", default=None, help="where the images are written")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the images of the run")
    parser.add_argument("--verbose", action="store_true", help="print the console of each boot")
    args = parser.parse_args(argv)

    reap_orphans()

    for path in (os.path.join(args.build, "loader32.elf"),
                 os.path.join(args.build, "microos64.bin"), args.mkfs):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build && make build", file=sys.stderr)
            return 2
    if shutil.which("sfdisk") is None:
        print("missing sfdisk: install fdisk", file=sys.stderr)
        return 2

    work = args.dir or tempfile.mkdtemp(prefix="microos-install-smoke-")
    os.makedirs(work, exist_ok=True)

    try:
        # The ext2 case boots the ordinary mfs image; the mfs case boots ext2
        # so the one MicroFS slot of the kernel is free for the target.
        for fs, live_fs in (("ext2", "mfs"), ("mfs", "ext2")):
            # A unique sentence in one file of /etc: the host finds those
            # bytes in the raw image and knows which byte is the file's.
            marker = (
                b"MICROOS-CORRUPT-ME-"
                + hashlib.sha256(os.urandom(32)).hexdigest()[:32].encode()
            )
            live = ensure_live(args.build, args.mkfs, live_fs, work, marker)
            target = os.path.join(work, f"install-{fs}.img")
            run_case(args.build, live, target, fs, marker, args.timeout, args.verbose)
    except (RuntimeError, OSError) as error:
        print(f"cannot prepare the images: {error}", file=sys.stderr)
        return 2
    finally:
        if not args.keep and args.dir is None:
            shutil.rmtree(work, ignore_errors=True)

    failed = [entry for entry in CHECKS if not entry[1]]
    for title, ok, detail in CHECKS:
        print(f"{'ok  ' if ok else 'FAIL'} {title}")
        if not ok and detail:
            print("     " + detail.strip().replace("\n", "\n     ")[:1200])

    print(f"{len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
