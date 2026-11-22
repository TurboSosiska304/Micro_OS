#!/usr/bin/env python3
"""Micro OS — the integrity check of the repository and of the firmware bundle.

The check has no dependencies beyond the Python standard library, so it runs
the same way on a developer laptop and on a CI machine:

    tools/integrity_check.py            # the whole check
    tools/integrity_check.py -v         # with the successful checks too
    tools/integrity_check.py --json report.json

What is checked:

    bundle     the files of dist/, their SHA-256 sums and the flash script
    image      the headers of the ESP images (magic, chip, alignment)
    partition  the partition table binary against partitions.csv and its MD5
    config     the flash size of sdkconfig.defaults against the images
    git        the integrity of the git objects
    sources    the hygiene of the tracked sources (LF, final newline, ...)
    format     the formatting of the C sources (clang-format)
    layers     the portable layers do not include the platform (ADR 0004)
    port       every function of the port contract has its implementation
    budget     the image of the kernel and its static DRAM stay inside the budget
    features   a feature that is off leaves no symbol in the map (ADR 0010)

The exit status is 0 when everything is in order and 1 otherwise.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import tarfile
import subprocess
import sys

# --- the constants of the ESP-IDF --------------------------------------------

ESP_IMAGE_MAGIC = 0xE9
ESP_PARTITION_MAGIC = 0x50AA
ESP_PARTITION_MD5_MARKER = 0xEBEB

# The size of the flash in the header of an ESP image (the upper nibble).
IMAGE_FLASH_SIZES = {0x0: 1, 0x1: 2, 0x2: 4, 0x3: 8, 0x4: 16}

# The types and the subtypes of a partition (esp_app_format.h).
PARTITION_TYPES = {"app": 0x00, "data": 0x01}

APP_SUBTYPES = {
    "factory": 0x00,
    "ota_0": 0x10,
    "ota_1": 0x11,
    "ota_2": 0x12,
    "ota_3": 0x13,
    "ota_4": 0x14,
    "ota_5": 0x15,
    "ota_6": 0x16,
    "ota_7": 0x17,
    "ota_8": 0x18,
    "ota_9": 0x19,
    "ota_10": 0x1A,
    "ota_11": 0x1B,
    "ota_12": 0x1C,
    "ota_13": 0x1D,
    "ota_14": 0x1E,
    "ota_15": 0x1F,
    "test": 0x20,
}

DATA_SUBTYPES = {
    "ota": 0x00,
    "phy": 0x01,
    "nvs": 0x02,
    "coredump": 0x03,
    "nvs_keys": 0x04,
    "efuse": 0x05,
    "undefined": 0x06,
    "esphttpd": 0x80,
    "fat": 0x81,
    "spiffs": 0x82,
    "littlefs": 0x83,
}

# The files of the bundle that must be ready to flash.
BUNDLE_BINARIES = ("micro_os.bin", "progs.bin", "bootloader.bin", "partition-table.bin")
BUNDLE_EXTRA = ("checksums.txt", "flash.sh", "ota_usb.py", "README.md")

# The directory of the `progs` partition (components/port_esp32/include/
# progs_format.h, ADR 0022).
PROGS_MAGIC = b"MOPROGS1"
PROGS_HEADER = 24
PROGS_ENTRY = 36
PROGS_SLOT = 0x10000
PROGS_DIRECTORY = 0x1000
PROGS_MAX_FILES = 2

# The extensions that are never treated as a text file.
BINARY_EXTENSIONS = {
    ".bin",
    ".sig",
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".ico",
    ".pdf",
    ".zip",
    ".gz",
    ".xz",
    ".woff",
    ".woff2",
    ".ttf",
}

# The maximum number of the offenders printed per check.
MAX_REPORTED = 10


# --- the report --------------------------------------------------------------


class Report:
    """The result of the check: a list of the checks with their verdict."""

    def __init__(self) -> None:
        self.checks = []

    def add(self, group: str, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
        return bool(ok)

    def skip(self, group: str, name: str, detail: str = "") -> None:
        self.checks.append({"group": group, "name": name, "ok": True, "detail": detail, "skipped": True})

    @property
    def failed(self) -> list:
        return [c for c in self.checks if not c["ok"]]

    @property
    def passed(self) -> int:
        return sum(1 for c in self.checks if c["ok"] and not c.get("skipped"))


# --- the helpers -------------------------------------------------------------


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def tracked_files(root: str) -> list:
    """Every file of the tree, including the ones not staged yet.

    The check runs on a laptop before a commit: a file that is not added yet
    is exactly the file nobody has looked at, so it must be checked too.
    """
    try:
        result = subprocess.run(
            ["git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return []

    if result.returncode != 0:
        return []

    return [line for line in result.stdout.splitlines() if line]

def walk_files(root: str) -> list:
    """Every file of the tree, without the build directories."""
    names = []
    for current, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIPPED_DIRS and not d.startswith("build-")]
        for name in files:
            path = os.path.join(current, name)
            names.append(os.path.relpath(path, root))
    return sorted(names)


def is_binary(path: str) -> bool:
    if os.path.splitext(path)[1].lower() in BINARY_EXTENSIONS:
        return True
    try:
        with open(path, "rb") as handle:
            return b"\0" in handle.read(8192)
    except OSError:
        return False


def parse_number(text: str) -> int:
    """A number of a partition table: 4096, 0x1000 or 0X1000."""
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    return int(text, 10)


def parse_sdkconfig(path: str) -> dict:
    """The interesting keys of an sdkconfig file."""
    config = {}
    if not os.path.isfile(path):
        return config
    for line in read_text(path).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        config[key.strip()] = value.strip().strip('"')
    return config


def flash_size_of(config: dict) -> int:
    """The size of the flash in megabytes, according to sdkconfig."""
    for key, value in config.items():
        if key.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_") and value == "y":
            name = key[len("CONFIG_ESPTOOLPY_FLASHSIZE_") :]
            if name.endswith("MB") and name[:-2].isdigit():
                return int(name[:-2])
    value = config.get("CONFIG_ESPTOOLPY_FLASHSIZE", "")
    if value.endswith("MB") and value[:-2].isdigit():
        return int(value[:-2])
    return 0


def parse_esp_image(path: str) -> dict:
    """The header of an ESP image (esp_image_header_t)."""
    with open(path, "rb") as handle:
        header = handle.read(24)

    if len(header) < 24:
        raise ValueError("the file is shorter than the header")

    magic = header[0]
    segments = header[1]
    spi_mode = header[2]
    spi_speed = header[3] & 0x0F
    spi_size = (header[3] >> 4) & 0x0F
    entry = struct.unpack("<I", header[4:8])[0]
    chip_id = struct.unpack("<H", header[12:14])[0]

    return {
        "magic": magic,
        "segments": segments,
        "spi_mode": spi_mode,
        "spi_speed": spi_speed,
        "spi_size": spi_size,
        "entry": entry,
        "chip_id": chip_id,
        "size": os.path.getsize(path),
    }


def parse_partition_table(path: str) -> dict:
    """The entries of a compiled partition table and its MD5 sum."""
    with open(path, "rb") as handle:
        data = handle.read()

    entries = []
    md5_entry = None
    offset = 0

    while offset + 32 <= len(data):
        raw = data[offset : offset + 32]
        magic = struct.unpack("<H", raw[:2])[0]

        if magic == ESP_PARTITION_MD5_MARKER:
            md5_entry = {
                "magic": magic,
                "digest": raw[16:32],
                "offset": offset,
            }
            offset += 32
            continue

        if magic != ESP_PARTITION_MAGIC:
            break

        ptype, psub = raw[2], raw[3]
        poff, psize = struct.unpack("<LL", raw[4:12])
        label = raw[12:28].split(b"\0")[0].decode("utf-8", "replace")
        flags = struct.unpack("<L", raw[28:32])[0]

        entries.append(
            {
                "name": label,
                "type": ptype,
                "subtype": psub,
                "offset": poff,
                "size": psize,
                "flags": flags,
            }
        )
        offset += 32

    digest = hashlib.md5(data[: offset - 32 if md5_entry else offset]).digest() if md5_entry else b""

    return {
        "entries": entries,
        "md5_entry": md5_entry,
        "md5_computed": digest,
        "padding": data[offset:],
    }


def parse_partitions_csv(path: str) -> list:
    """The partitions of partitions.csv, the way ESP-IDF reads them."""
    partitions = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.split("#")[0].strip()
            if not line:
                continue

            fields = [f.strip() for f in line.split(",")]
            if len(fields) < 5:
                raise ValueError(f"{path}:{number}: expected 5 fields, got {len(fields)}")

            name, ptype, psub = fields[0], fields[1], fields[2]
            offset = parse_number(fields[3])
            size = parse_number(fields[4])

            if ptype.startswith("0x"):
                type_id = int(ptype, 16)
            elif ptype in PARTITION_TYPES:
                type_id = PARTITION_TYPES[ptype]
            else:
                raise ValueError(f"{path}:{number}: unknown type {ptype!r}")

            if psub.startswith("0x"):
                sub_id = int(psub, 16)
            elif ptype == "app" and psub in APP_SUBTYPES:
                sub_id = APP_SUBTYPES[psub]
            elif ptype == "data" and psub in DATA_SUBTYPES:
                sub_id = DATA_SUBTYPES[psub]
            else:
                raise ValueError(f"{path}:{number}: unknown subtype {psub!r}")

            partitions.append(
                {
                    "name": name,
                    "type": type_id,
                    "subtype": sub_id,
                    "offset": offset,
                    "size": size,
                }
            )

    return partitions


# --- the checks --------------------------------------------------------------


def check_bundle(root: str, report: Report) -> dict:
    """The files of dist/: their presence, their sums and the flash script."""
    group = "bundle"
    dist = os.path.join(root, "dist")

    # The bundle is a product of the build, not a file of the repository: a
    # fresh checkout has no dist/ at all and that is not a failure. The CI
    # builds the firmware in its own job and checks the fresh bundle there,
    # with this same script and --skip-git --skip-format.
    if not os.path.isdir(dist):
        report.skip(group, "the bundle is built", "dist/ is not there")
        return {}

    report.add(group, "dist/ exists", True, dist)

    for name in BUNDLE_BINARIES:
        path = os.path.join(dist, name)
        exists = os.path.isfile(path) and os.path.getsize(path) > 0
        report.add(
            group,
            f"dist/{name} is present",
            exists,
            "" if exists else "the file is missing or empty",
        )

    for name in BUNDLE_EXTRA:
        path = os.path.join(dist, name)
        report.add(group, f"dist/{name} is present", os.path.isfile(path), "")

    # --- the sums
    sums_path = os.path.join(dist, "checksums.txt")
    sums = {}

    if os.path.isfile(sums_path):
        for number, line in enumerate(read_text(sums_path).splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) != 2:
                report.add(group, "checksums.txt is parsable", False, f"line {number}: {line!r}")
                continue
            digest, name = parts[0].lower(), parts[1].strip().lstrip("*")
            sums[name] = digest

        # The single image of the board is a binary of the bundle too, and its
        # name carries the version: the folder says which one is expected.
        images = {
            os.path.basename(path) for path in glob.glob(os.path.join(dist, "MicroOS-*.bin"))
        }
        tarballs = {
            os.path.basename(path)
            for path in glob.glob(os.path.join(dist, "MicroOS-x86_64-*.tar.gz"))
        }
        # The signature trailer and the public key are parts of the bundle
        # (ADR 0020), and so is the tarball of the other machine.
        signature = {
            name
            for name in os.listdir(dist)
            if name.endswith(".sig") or name == "microos-sign.pub"
        }
        expected = set(BUNDLE_BINARIES) | images | tarballs | signature

        report.add(
            group,
            "checksums.txt covers every binary",
            set(sums) == expected,
            f"expected {sorted(expected)}, got {sorted(sums)}",
        )

        for name, expected in sums.items():
            path = os.path.join(dist, name)
            if not os.path.isfile(path):
                report.add(group, f"the sum of {name} matches", False, "the file is missing")
                continue
            actual = sha256_of(path)
            report.add(
                group,
                f"the sum of {name} matches",
                actual == expected,
                "" if actual == expected else f"expected {expected}, got {actual}",
            )
    else:
        report.add(group, "checksums.txt is present", False, "the file is missing")

    # --- the flash script
    flash = os.path.join(dist, "flash.sh")
    if os.path.isfile(flash):
        report.add(
            group,
            "dist/flash.sh is executable",
            os.access(flash, os.X_OK),
            "chmod +x dist/flash.sh",
        )
        script = read_text(flash)
        missing = [n for n in BUNDLE_BINARIES if n not in script]
        report.add(
            group,
            "dist/flash.sh flashes every binary",
            not missing,
            "" if not missing else f"not mentioned: {', '.join(missing)}",
        )
    else:
        report.add(group, "dist/flash.sh is present", False, "the file is missing")

    return sums


X86_ARTIFACTS = ("loader32.elf", "microos64.bin", "run.sh", "README.md")


def app_slot_offset(root: str) -> int:
    """The offset of the first application slot, as partitions.csv says it."""
    path = os.path.join(root, "partitions.csv")
    if not os.path.isfile(path):
        return 0x10000

    for line in read_text(path).splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 4 and fields[0] == "ota_0":
            try:
                return int(fields[3], 0)
            except ValueError:
                return 0x10000

    return 0x10000


def check_artifacts(root: str, report: Report) -> None:
    """The bundle carries both machines: a single image and a tarball."""
    group = "bundle"
    dist = os.path.join(root, "dist")

    if not os.path.isdir(dist):
        report.skip(group, "the artifacts of both machines", "dist/ is not there")
        return

    merged = sorted(glob.glob(os.path.join(dist, "MicroOS-*.bin")))
    app = os.path.join(dist, "micro_os.bin")

    if not merged:
        report.add(group, "the single image is there", False, "no MicroOS-*.bin in dist/")
    else:
        data = open(merged[0], "rb").read()
        detail = f"{os.path.basename(merged[0])}, {len(data)} bytes"
        ok = len(data) > 0x1000 and data[0x1000] == ESP_IMAGE_MAGIC
        if ok and os.path.isfile(app):
            application = open(app, "rb").read()
            offset = app_slot_offset(root)
            ok = (
                len(data) >= offset + len(application)
                and data[offset : offset + len(application)] == application
            )
            detail = (
                "the application at " + hex(offset) + " is micro_os.bin"
                if ok
                else "the application inside the image is not micro_os.bin"
            )
        report.add(group, "the single image carries the application", ok, detail)

    tarballs = sorted(glob.glob(os.path.join(dist, "MicroOS-x86_64-*.tar.gz")))
    if not tarballs:
        report.add(group, "the bundle of the x86-64 port is there", False, "no tarball in dist/")
        return

    try:
        with tarfile.open(tarballs[0], "r:gz") as archive:
            names = set(archive.getnames())
    except (OSError, tarfile.TarError) as error:
        report.add(group, "the bundle of the x86-64 port is readable", False, str(error))
        return

    missing = [name for name in X86_ARTIFACTS if name not in names]
    report.add(
        group,
        "the bundle of the x86-64 port has what it needs",
        not missing,
        "missing: " + ", ".join(missing) if missing else ", ".join(X86_ARTIFACTS),
    )


def check_images(root: str, report: Report) -> dict:
    """The headers of the ESP images."""
    group = "image"
    dist = os.path.join(root, "dist")
    images = {}

    if not os.path.isdir(dist):
        report.skip(group, "the images are read", "dist/ is not there")
        return images

    for name in ("micro_os.bin", "bootloader.bin"):
        path = os.path.join(dist, name)
        if not os.path.isfile(path):
            report.add(group, f"{name} can be read", False, "the file is missing")
            continue

        try:
            header = parse_esp_image(path)
        except (OSError, ValueError) as error:
            report.add(group, f"{name} can be read", False, str(error))
            continue

        images[name] = header

        report.add(
            group,
            f"{name} has the ESP image magic",
            header["magic"] == ESP_IMAGE_MAGIC,
            f"magic is 0x{header['magic']:02X}, expected 0x{ESP_IMAGE_MAGIC:02X}",
        )
        report.add(
            group,
            f"{name} is an ESP32 image",
            header["chip_id"] == 0x0000,
            f"chip id is 0x{header['chip_id']:04X}, expected 0x0000",
        )
        report.add(
            group,
            f"{name} has a sane segment count",
            1 <= header["segments"] <= 16,
            f"{header['segments']} segment(s)",
        )
        report.add(
            group,
            f"{name} is aligned to 16 bytes",
            header["size"] % 16 == 0,
            f"{header['size']} bytes",
        )

    return images


def check_progs(root: str, report: Report) -> None:
    """The image of the programs: the directory of the progs partition."""
    group = "progs"
    dist = os.path.join(root, "dist")
    path = os.path.join(dist, "progs.bin")

    if not os.path.isdir(dist):
        report.skip(group, "the programs are read", "dist/ is not there")
        return

    if not os.path.isfile(path):
        report.add(group, "progs.bin is present", False, "the file is missing")
        return

    data = open(path, "rb").read()
    report.add(group, "progs.bin is present", True, f"{len(data)} bytes")

    expected = 0x21000
    csv_path = os.path.join(root, "partitions.csv")
    if os.path.isfile(csv_path):
        for partition in parse_partitions_csv(csv_path):
            if partition["name"] == "progs":
                expected = partition["size"]
                break

    report.add(
        group,
        "progs.bin is the partition of the table",
        len(data) == expected,
        f"{len(data)} bytes, the partition is {expected}",
    )

    if len(data) < PROGS_DIRECTORY:
        report.add(group, "the directory is readable", False, "the image is too short")
        return

    directory = len(data) - PROGS_DIRECTORY
    table = data[directory:]
    ok = table[0:8] == PROGS_MAGIC
    report.add(group, "the directory has the magic", ok, table[0:8].decode("ascii", "replace"))

    version, count, slot, _reserved = struct.unpack_from("<IIII", table, 8)
    report.add(group, "the directory is version 1", version == 1, f"version {version}")

    report.add(
        group,
        "the directory fits the partition",
        count <= PROGS_MAX_FILES and slot >= PROGS_SLOT and slot % PROGS_SLOT == 0,
        f"{count} program(s), slot 0x{slot:X}",
    )

    for index in range(min(count, PROGS_MAX_FILES)):
        record = PROGS_HEADER + index * PROGS_ENTRY
        if record + PROGS_ENTRY > len(table):
            report.add(group, f"program {index} is in the image", False, "the record is missing")
            continue

        name = table[record : record + 24].split(b"\0", 1)[0].decode("ascii", "replace")
        offset, size, _ = struct.unpack_from("<III", table, record + 24)
        ok = (
            name != ""
            and size > 0
            and size <= slot
            and offset % slot == 0
            and offset + size <= directory
        )
        report.add(
            group,
            f"the record of {name or index} is usable",
            ok,
            f"0x{offset:X}, {size} bytes",
        )


def check_partitions(root: str, report: Report) -> dict:
    """The partition table binary against partitions.csv."""
    group = "partition"
    dist = os.path.join(root, "dist")
    binary = os.path.join(dist, "partition-table.bin")
    csv_path = os.path.join(root, "partitions.csv")

    if not os.path.isdir(dist):
        report.skip(group, "the partition table is read", "dist/ is not there")
        return {}

    if not os.path.isfile(binary):
        report.add(group, "partition-table.bin is present", False, "the file is missing")
        return {}

    table = parse_partition_table(binary)

    if not table["entries"]:
        report.add(group, "the partition table has entries", False, "no entry found")
        return table

    report.add(
        group,
        "the partition table has entries",
        True,
        f"{len(table['entries'])} partition(s)",
    )

    # --- the MD5 of the table
    if table["md5_entry"] is None:
        report.add(group, "the partition table has the MD5 entry", False, "the entry is missing")
    else:
        report.add(
            group,
            "the partition table has the MD5 entry",
            True,
            "",
        )
        report.add(
            group,
            "the MD5 of the partition table matches",
            table["md5_entry"]["digest"] == table["md5_computed"],
            ""
            if table["md5_entry"]["digest"] == table["md5_computed"]
            else "the table is damaged",
        )

    # --- the padding after the table is the end marker of the ESP-IDF
    padding = table["padding"]
    report.add(
        group,
        "the partition table ends with the end marker",
        not padding or all(byte == 0xFF for byte in padding),
        "the padding after the table is not 0xFF" if padding else "",
    )

    # --- the table against the source
    if not os.path.isfile(csv_path):
        report.add(group, "partitions.csv is present", False, "the file is missing")
        return table

    try:
        expected = parse_partitions_csv(csv_path)
    except (OSError, ValueError) as error:
        report.add(group, "partitions.csv is parsable", False, str(error))
        return table

    report.add(group, "partitions.csv is parsable", True, f"{len(expected)} partition(s)")

    for want in expected:
        found = None
        for entry in table["entries"]:
            if entry["name"] == want["name"]:
                found = entry
                break

        if found is None:
            report.add(
                group,
                f"the partition {want['name']} is in the table",
                False,
                "the partition is missing",
            )
            continue

        same = (
            found["type"] == want["type"]
            and found["subtype"] == want["subtype"]
            and found["offset"] == want["offset"]
            and found["size"] == want["size"]
        )
        report.add(
            group,
            f"the partition {want['name']} matches the source",
            same,
            ""
            if same
            else (
                f"source: type=0x{want['type']:02X} subtype=0x{want['subtype']:02X} "
                f"offset=0x{want['offset']:X} size=0x{want['size']:X}, "
                f"table: type=0x{found['type']:02X} subtype=0x{found['subtype']:02X} "
                f"offset=0x{found['offset']:X} size=0x{found['size']:X}"
            ),
        )

    extra = [e["name"] for e in table["entries"] if e["name"] not in {p["name"] for p in expected}]
    report.add(
        group,
        "the table has no extra partitions",
        not extra,
        "" if not extra else f"extra: {', '.join(extra)}",
    )

    # --- the partitions do not overlap
    ordered = sorted(table["entries"], key=lambda e: e["offset"])
    overlap = []
    for first, second in zip(ordered, ordered[1:]):
        if first["offset"] + first["size"] > second["offset"]:
            overlap.append(f"{first['name']} and {second['name']}")
    report.add(
        group,
        "the partitions do not overlap",
        not overlap,
        "" if not overlap else f"overlap: {', '.join(overlap)}",
    )

    return table


def check_config(root: str, report: Report, images: dict, table: dict) -> None:
    """The flash size of the configuration against the images and the table."""
    group = "config"
    config = parse_sdkconfig(os.path.join(root, "sdkconfig.defaults"))

    if not config:
        report.add(group, "sdkconfig.defaults is present", False, "the file is missing or empty")
        return

    report.add(group, "sdkconfig.defaults is present", True, "")

    target = config.get("CONFIG_IDF_TARGET", "")
    report.add(
        group,
        "the target is esp32",
        target == "esp32",
        f"CONFIG_IDF_TARGET is {target!r}",
    )

    size_mb = flash_size_of(config)
    report.add(
        group,
        "the size of the flash is known",
        size_mb > 0,
        f"{size_mb} MB" if size_mb else "CONFIG_ESPTOOLPY_FLASHSIZE_* is not set",
    )

    if size_mb <= 0:
        return

    limit = size_mb * 1024 * 1024

    # --- the images
    for name, header in images.items():
        image_mb = IMAGE_FLASH_SIZES.get(header["spi_size"])
        report.add(
            group,
            f"{name} was built for the flash of {size_mb} MB",
            image_mb == size_mb,
            ""
            if image_mb == size_mb
            else f"the image says {image_mb} MB, the configuration says {size_mb} MB",
        )

    # --- the table
    for entry in table.get("entries", []):
        end = entry["offset"] + entry["size"]
        report.add(
            group,
            f"the partition {entry['name']} fits into the flash",
            end <= limit,
            f"it ends at 0x{end:X}, the flash is 0x{limit:X} bytes",
        )


def git_directory(root: str) -> str:
    """The directory of the git objects of this checkout.

    A linked worktree has a .git *file* that names the real directory, so a
    test of os.path.isdir(root/.git) skips the check exactly where a review
    runs. `git rev-parse --git-dir` answers for both: a normal clone says
    .git, which keeps the command of that case the same.
    """
    try:
        result = subprocess.run(
            ["git", "-C", root, "rev-parse", "--git-dir"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return ""

    if result.returncode != 0:
        return ""

    path = result.stdout.strip()
    if not path:
        return ""

    # rev-parse prints a relative path (".git") in a clone and an absolute
    # one in a worktree; git is asked from the root, so the join is right.
    return path if os.path.isabs(path) else os.path.join(root, path)


def check_git(root: str, report: Report) -> None:
    """The integrity of the git objects."""
    group = "git"

    directory = git_directory(root)
    if not directory or not os.path.isdir(directory):
        report.skip(group, "git fsck", "the directory is not a git repository")
        return

    try:
        result = subprocess.run(
            ["git", "-C", root, "--git-dir", directory, "fsck", "--strict", "--no-progress"],
            capture_output=True,
            text=True,
        )
    except OSError as error:
        report.skip(group, "git fsck", str(error))
        return

    output = (result.stdout + result.stderr).strip()
    # The dangling objects are a notice, not an error: a dangling blob is a
    # commit that is not referenced by any branch, the objects are intact.
    troubles = ("error", "missing", "corrupt", "broken", "invalid")
    errors = [
        line
        for line in output.splitlines()
        if line.strip() and any(word in line.lower() for word in troubles)
    ]

    report.add(
        group,
        "the git objects are intact",
        result.returncode == 0 and not errors,
        "" if not errors else "; ".join(errors[:5]),
    )


def check_sources(root: str, report: Report, files: list) -> None:
    """The hygiene of the tracked sources (the rules of .editorconfig)."""
    group = "sources"

    crlf = []
    no_newline = []
    trailing = []
    tabs = []
    too_long = []
    not_utf8 = []

    for name in files:
        path = os.path.join(root, name)
        if name.startswith("third_party/"):
            # The vendored sources keep the style of their upstream (sbase is
            # tabs and 80 columns, zlib is its own). Their license and their
            # README are ours and are checked as every other file.
            continue
        if not os.path.isfile(path) or is_binary(path):
            continue

        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError:
            continue

        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            not_utf8.append(name)
            continue

        if b"\r\n" in data:
            crlf.append(name)

        if data and not data.endswith(b"\n"):
            no_newline.append(name)

        lines = data.split(b"\n")

        if not name.endswith(".md"):
            for number, line in enumerate(lines, 1):
                if line.rstrip(b" \t\r") != line:
                    trailing.append(f"{name}:{number}")
                    break

        is_makefile = name == "Makefile" or name.endswith(".mk")
        if not is_makefile:
            for number, line in enumerate(lines, 1):
                if b"\t" in line:
                    tabs.append(f"{name}:{number}")
                    break

        if name.endswith((".c", ".h")):
            for number, line in enumerate(lines, 1):
                if len(line) > 100:
                    too_long.append(f"{name}:{number} ({len(line)} characters)")
                    break

    def detail(items: list) -> str:
        shown = ", ".join(items[:MAX_REPORTED])
        if len(items) > MAX_REPORTED:
            shown += f" and {len(items) - MAX_REPORTED} more"
        return shown

    report.add(
        group,
        "the sources are UTF-8",
        not not_utf8,
        detail(not_utf8),
    )
    report.add(
        group,
        "the sources use LF",
        not crlf,
        detail(crlf),
    )
    report.add(
        group,
        "the sources end with a newline",
        not no_newline,
        detail(no_newline),
    )
    report.add(
        group,
        "the sources have no trailing whitespace",
        not trailing,
        detail(trailing),
    )
    report.add(
        group,
        "the sources are indented with spaces",
        not tabs,
        detail(tabs),
    )
    report.add(
        group,
        "the C sources are at most 100 characters long",
        not too_long,
        detail(too_long),
    )


def check_format(root: str, report: Report, files: list, formatter: str) -> None:
    """The formatting of the C sources."""
    group = "format"

    sources = [f for f in files if f.endswith((".c", ".h"))]
    if not sources:
        report.skip(group, "clang-format", "there are no C sources")
        return

    if not shutil.which(formatter):
        report.skip(group, "clang-format", f"{formatter} is not installed")
        return

    try:
        result = subprocess.run(
            [formatter, "--dry-run", "--Werror", *sources],
            cwd=root,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        report.skip(group, "clang-format", str(error))
        return

    output = (result.stdout + result.stderr).strip()
    lines = [line for line in output.splitlines() if line.strip()]
    detail = "; ".join(lines[:MAX_REPORTED])
    if len(lines) > MAX_REPORTED:
        detail += f" and {len(lines) - MAX_REPORTED} more"

    report.add(
        group,
        f"the C sources are formatted ({formatter})",
        result.returncode == 0,
        detail,
    )


# --- the layers of the tree --------------------------------------------------
#
# The kernel is plain C11 and the portable components are portable C: a
# header of ESP-IDF or of the board that appears where it must not is the
# boundary going away (ADR 0004). It is cheapest to catch that here, on a
# laptop, than on a board that no longer builds.

KERNEL_DIR = "components/kernel/"

PORTABLE_DIRS = (
    "components/board/",
    "components/config/",
    "components/devfs/",
    "components/font/",
    "components/graphics/",
    "components/init/",
    "components/kshell/",
    "components/libc/",
    "components/mfs/",
    "components/net/",
    "components/procfs/",
    "components/telnet/",
    "components/wifi/",
)

# What the kernel core may not include at all: the chip, the board, the
# upper layers, the port implementations, and POSIX (the host is a port,
# the kernel is not).
KERNEL_FORBIDDEN = re.compile(
    r'#\s*include\s*[<"]((?:'
    r"esp_[a-z0-9_]*\.h|freertos/|driver/|drivers/|hal/|soc/|rom/|"
    r"nvs[a-z_]*\.h|lwip/|board/|kshell/|port_esp32\.h|port_host\.h|"
    r"unistd\.h|pthread\.h|sys/"
    r"))"
)

# What a portable component may not include.
PLATFORM_INCLUDE = re.compile(
    r'#\s*include\s*[<"]((?:'
    r"esp_[a-z0-9_]*\.h|freertos/|driver/|hal/|soc/|rom/|"
    r"nvs[a-z_]*\.h|lwip/|port_esp32\.h|port_host\.h"
    r"))"
)

# The platform files that still live inside portable components. Every
# entry is a debt the portability work pays back; the check fails when an
# entry is no longer needed, so the list can only shrink.
PLATFORM_DEBT = {
}

# A portable component that still speaks to the port directly. The same
# rule as above, for the calls instead of the includes: the upper layers
# ask the kernel (kernel/sysinfo.h, kernel/lock.h), never k_port_*.
PORT_CALL_DEBT = {
}

PORT_CALL = re.compile(r"\b(k_port_[a-z0-9_]+)\s*\(")


# What the kernel core may not include at all: the chip, the board, the
# upper layers, the port implementations, and POSIX (the host is a port, the
# kernel is not).
KERNEL_FORBIDDEN = (
    r"esp_[a-z0-9_]*\.h",
    r"freertos/",
    r"driver/",
    r"drivers/",
    r"hal/",
    r"soc/",
    r"rom/",
    r"nvs[a-z_]*\.h",
    r"lwip/",
    r"board/",
    r"kshell/",
    r"port_esp32\.h",
    r"port_host\.h",
    r"unistd\.h",
    r"pthread\.h",
    r"sys/",
)

# What a portable component may not include. Its own directory is not here:
# components/board includes board/board.h, and that is its own header.
PLATFORM_INCLUDE = (
    r"esp_[a-z0-9_]*\.h",
    r"freertos/",
    r"driver/",
    r"hal/",
    r"soc/",
    r"rom/",
    r"nvs[a-z_]*\.h",
    r"lwip/",
    r"kernel/port",
    r"drivers/",
    r"board/",
    r"port_esp32\.h",
    r"port_host\.h",
    r"^pthread\.h$",
    r"^unistd\.h$",
    r"^sys/",
)

# The platform files that still live inside portable components. Every entry
# is a debt the portability work pays back; the check fails when an entry is
# no longer needed, so the list can only shrink.
PLATFORM_DEBT = {}

# A portable component that still speaks to the port directly. The same rule,
# for the calls: an identifier is a use, not only "name(".
PORT_CALL_DEBT = {}

INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
PORT_CALL = re.compile(r"\bk_port_[a-z0-9_]+\b")
IDENTIFIER = re.compile(r"[A-Za-z0-9_./-]+")


def code_only(text: str) -> str:
    """Drop comments and the backslash of a continued line.

    A comment that names a function is documentation, not a call; a header
    included through a macro is still found by the text of the macro, and a
    line continued with a backslash does not hide the include from the
    pattern. The strings are kept: a call spelled in a string is not a call,
    but a header spelled in a string is not either - and both are rare
    enough to accept.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text.replace("\\\n", " ")


def check_layers(root: str, report: Report, files: list) -> None:
    """The portable layers do not include the platform (ADR 0004)."""
    group = "layers"

    kernel_bad = []
    portable_bad = []
    debt_left = set(PLATFORM_DEBT)
    call_bad = []
    call_debt_left = set(PORT_CALL_DEBT)

    for name in files:
        path = os.path.join(root, name)
        if name.startswith("third_party/"):
            # The vendored sources keep the style of their upstream (sbase is
            # tabs and 80 columns, zlib is its own). Their license and their
            # README are ours and are checked as every other file.
            continue
        if name.startswith("components/libc/user/"):
            # The library of a program is the one component that is POSIX on
            # purpose: its <unistd.h>, <sys/stat.h> and <pwd.h> are its own
            # headers in `include/`, and a portable source that includes them
            # is the point of ADR 0016.
            continue
        if not os.path.isfile(path) or is_binary(path):
            continue

        text = code_only(read_text(path))

        if name.startswith(KERNEL_DIR):
            for match in INCLUDE.finditer(text):
                header = match.group(1)
                for pattern in KERNEL_FORBIDDEN:
                    if re.search(pattern, header):
                        kernel_bad.append(f"{name}: {header}")
                        break
        elif name.startswith(PORTABLE_DIRS):
            component = name.split("/")[1]

            for match in INCLUDE.finditer(text):
                header = match.group(1)

                # The header of the component itself is not a platform header.
                if header.startswith(component + "/"):
                    continue

                for pattern in PLATFORM_INCLUDE:
                    if re.search(pattern, header):
                        debt_left.discard(name)
                        if name not in PLATFORM_DEBT:
                            portable_bad.append(f"{name}: {header}")
                        break

            for match in PORT_CALL.finditer(text):
                call_debt_left.discard(name)
                if name not in PORT_CALL_DEBT:
                    call_bad.append(f"{name}: {match.group(0)}")
                    break

    report.add(
        group,
        "the kernel includes nothing but the kernel",
        not kernel_bad,
        "; ".join(kernel_bad[:5]) if kernel_bad else "no platform header in " + KERNEL_DIR,
    )
    report.add(
        group,
        "the portable components include no platform code",
        not portable_bad,
        "; ".join(portable_bad[:5]) if portable_bad else f"{len(PORTABLE_DIRS)} directory(ies)",
    )
    report.add(
        group,
        "the list of the platform debt is up to date",
        not debt_left,
        "; ".join(f"{name}: {PLATFORM_DEBT[name]} - remove the entry" for name in sorted(debt_left))
        if debt_left
        else f"{len(PLATFORM_DEBT)} known file(s)",
    )
    report.add(
        group,
        "the portable components do not call the port",
        not call_bad,
        "; ".join(call_bad[:5]) if call_bad else f"{len(PORT_CALL_DEBT)} known caller(s)",
    )
    report.add(
        group,
        "the list of the port callers is up to date",
        not call_debt_left,
        "; ".join(
            f"{name}: {PORT_CALL_DEBT[name]} - remove the entry" for name in sorted(call_debt_left)
        )
        if call_debt_left
        else f"{len(PORT_CALL_DEBT)} known caller(s)",
    )
# --- the contract of the port -------------------------------------------------

PORT_HEADER_DIR = "components/kernel/include/kernel/port"
PORT_IMPLEMENTATION_DIRS = (
    "components/port_esp32/src",
    "ports/port_host/src",
    "ports/port_null/src",
)

# A declaration of the contract: a line that starts with the return type and
# names a k_port_* function. The comments of the headers never match.
CONTRACT_DECLARATION = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(k_port_[a-z0-9_]+)\s*\(", re.MULTILINE)


def contract_functions(root: str) -> list:
    directory = os.path.join(root, PORT_HEADER_DIR)
    if not os.path.isdir(directory):
        return []

    names = set()
    for entry in sorted(os.listdir(directory)):
        if entry.endswith(".h"):
            names.update(CONTRACT_DECLARATION.findall(read_text(os.path.join(directory, entry))))

    return sorted(names)


def check_portable_build(root: str, report: Report) -> None:
    """The host build compiles every portable component."""
    group = "layers"

    path = os.path.join(root, "CMakeLists.txt")
    if not os.path.isfile(path):
        report.add(group, "the top level build file is there", False, "CMakeLists.txt is missing")
        return

    # CMake comments start with "#" and a commented add_subdirectory used to
    # satisfy the check while the component was never compiled.
    text = re.sub(r"^\s*#.*$", "", read_text(path), flags=re.M)
    missing = []
    unbuilt = []

    for directory in PORTABLE_DIRS:
        name = directory.rstrip("/")

        if f"add_subdirectory({name})" not in text:
            missing.append(name)
            continue

        # A component that is added but whose library is not in the build is
        # still not compiled: a commented line used to satisfy the text.
        library_dir = os.path.join(root, "build-host", "components", os.path.basename(name))
        if os.path.isdir(library_dir) and not any(
            entry.startswith("libmicroos_") and entry.endswith(".a")
            for entry in os.listdir(library_dir)
        ):
            unbuilt.append(name)

    report.add(
        group,
        "the host build includes every portable component",
        not missing,
        "not built on the host: " + ", ".join(missing) if missing else f"{len(PORTABLE_DIRS)} component(s)",
    )
    report.add(
        group,
        "the libraries of the portable components are there",
        not unbuilt,
        "no library: " + ", ".join(unbuilt) if unbuilt else "one library per component",
    )

# The order of the directories is the order of the report; a port without a
# host build (the ESP32) is checked by the text of its sources.
PORT_IMPLEMENTATION_DIRS = (
    "components/port_esp32/src",
    "ports/port_host/src",
    "ports/port_null/src",
    "ports/port_x86_64/src",
)

# Where the host build puts the libraries of the ports that it builds.
PORT_LIBRARIES = {
    "ports/port_host/src": "build-host/ports/port_host/libmicroos_port_host.a",
    "ports/port_null/src": "build-host/ports/port_null/libmicroos_port_null.a",
}


def contract_functions(root: str) -> list:
    directory = os.path.join(root, PORT_HEADER_DIR)
    if not os.path.isdir(directory):
        return []

    names = set()
    for entry in sorted(os.listdir(directory)):
        if entry.endswith(".h"):
            names.update(CONTRACT_DECLARATION.findall(read_text(os.path.join(directory, entry))))

    return sorted(names)


def defined_symbols(path: str) -> set:
    """The global symbols an archive defines, if nm is there to say."""
    try:
        result = subprocess.run(
            ["nm", "--defined-only", "--extern-only", path],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return set()

    if result.returncode != 0:
        return set()

    symbols = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t", "D", "d", "B", "b", "R", "r"):
            symbols.add(parts[2])

    return symbols


def check_port_contract(root: str, report: Report) -> None:
    """Every function of the contract has an implementation in every port."""
    group = "port"
    names = contract_functions(root)

    report.add(
        group,
        "the contract declares its functions",
        bool(names),
        f"{len(names)} function(s) in {PORT_HEADER_DIR}/",
    )

    # The calls with a weak default in the kernel (kernel/port/cpu.h) are not
    # required of a port that predates them: the kernel answers one core and
    # the port keeps working (ADR 0012). A port that has them is checked for
    # them like any other call.
    weak = weak_calls(root)
    required = [name for name in names if name not in weak]

    for directory in PORT_IMPLEMENTATION_DIRS:
        absent, where = port_missing(root, directory, required)

        if absent is None:
            report.add(group, f"{directory} exists", False, where)
            continue

        detail = (
            "missing: " + ", ".join(absent)
            if absent
            else f"{len(required)} function(s) found in {where}"
        )
        report.add(group, f"{directory} implements the contract", not absent, detail)


# The record of the contract: the functions of kernel/port/*.h as the release
# froze them. It is the list a port is measured against, so a function that is
# dropped from the headers without a line here - or added without one - is a
# failed check and not a quiet change of the interface.
PORT_RECORDING = "tests/corpus/port_api.txt"

# The two ports that are machines of their own: the board and the x86-64 port
# of QEMU. The host and the null port are the cheap checks of the contract;
# the gate of the release asks for the whole list from two independent
# machines (docs/releases/1.0.md, section 1).
PORT_INDEPENDENT_DIRS = (
    "components/port_esp32/src",
    "ports/port_x86_64/src",
)


def port_version(root: str) -> tuple:
    """The version of the contract in kernel/port/info.h, as (major, minor)."""
    path = os.path.join(root, PORT_HEADER_DIR, "info.h")
    if not os.path.isfile(path):
        return None

    text = read_text(path)
    values = []
    for name in ("MAJOR", "MINOR"):
        match = re.search(
            r"#define\s+KERNEL_PORT_API_VERSION_%s\s+(\d+)" % name,
            text,
        )
        if match is None:
            return None
        values.append(match.group(1))

    return tuple(values)


def read_port_recording(root: str) -> tuple:
    """The record of the contract: the version, the functions and the header of each.

    Returns (version, names, optional, errors). A version is a pair of strings,
    the names are a dictionary from the function to the header that declares
    it, `optional` holds the functions a port may leave to the weak default of
    the kernel (kernel/cpu.h), and the errors are the lines that are neither.
    """
    path = os.path.join(root, PORT_RECORDING)
    version = None
    names = {}
    optional = set()
    errors = []

    if not os.path.isfile(path):
        return None, names, optional, [f"{PORT_RECORDING} is missing"]

    for number, line in enumerate(read_text(path).splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue

        parts = line.split()

        if parts[0] == "api" and len(parts) == 3:
            if version is not None:
                errors.append(f"line {number}: a second api line")
            version = (parts[1], parts[2])
        elif parts[0].startswith("k_port_") and len(parts) in (2, 3):
            if parts[0] in names:
                errors.append(f"line {number}: {parts[0]} is recorded twice")
            names[parts[0]] = parts[1]
            if len(parts) == 3:
                if parts[2] != "weak":
                    errors.append(f"line {number}: {parts[2]!r} is not a word of the record")
                else:
                    optional.add(parts[0])
        else:
            errors.append(f"line {number}: {line!r} is not a record")

    return version, names, optional, errors


def port_missing(root: str, directory: str, names: list) -> tuple:
    """The functions of `names` a port does not implement, and where they were looked for.

    The answer is (missing, detail): the host build of a port is read through
    the symbols of its library, a port without one through the text of its
    sources. `missing` is None when the directory is not there at all.
    """
    path = os.path.join(root, directory)
    if not os.path.isdir(path):
        return None, "the directory is missing"

    library = PORT_LIBRARIES.get(directory)
    symbols = defined_symbols(os.path.join(root, library)) if library else set()

    if symbols:
        return [name for name in names if name not in symbols], "the library"

    # No host build of this port (the ESP32, the x86-64 machine): the text of
    # its sources. code_only drops the comments, so a name in a comment is not
    # a function that was written.
    text = ""
    for entry in sorted(os.listdir(path)):
        if entry.endswith((".c", ".S")):
            text += code_only(read_text(os.path.join(path, entry)))

    missing = [name for name in names if not re.search(r"\b" + name + r"\s*\(", text)]
    return missing, "the sources"


def weak_calls(root: str) -> set:
    """The calls of the record that a port may leave to the kernel.

    The record marks them with `weak`: the portable kernel has a definition
    (components/kernel/src/cpu_default.c) and a port that does not override it
    is one core. The board is that port until its own work (ADR 0012).
    """
    _version, _names, optional, _errors = read_port_recording(root)
    return optional


def check_port_recording(root: str, report: Report) -> list:
    """The record of the contract is the one of the headers, and it is whole."""
    group = "port"
    version, names, optional, errors = read_port_recording(root)

    report.add(
        group,
        f"the contract is recorded ({PORT_RECORDING})",
        not errors,
        "; ".join(errors[:MAX_REPORTED]),
    )

    declared = contract_functions(root)
    recorded = sorted(names)

    missing = [name for name in declared if name not in names]
    extra = [name for name in recorded if name not in declared]
    detail = []
    if missing:
        detail.append("declared and not recorded: " + ", ".join(missing))
    if extra:
        detail.append("recorded and not declared: " + ", ".join(extra))

    report.add(
        group,
        "the record is the list of the headers",
        not detail,
        "; ".join(detail) if detail else f"{len(recorded)} function(s)",
    )

    # A record names the header of its function: the file is a map of the
    # contract, not a list of names without a place.
    wrong = []
    for name, header in sorted(names.items()):
        path = os.path.join(root, "components/kernel/include", header)
        if not os.path.isfile(path):
            wrong.append(f"{name}: {header} is not a header")
            continue
        if name not in CONTRACT_DECLARATION.findall(read_text(path)):
            wrong.append(f"{name}: not declared in {header}")

    report.add(
        group,
        "every function is recorded with the header that declares it",
        not wrong,
        "; ".join(wrong[:MAX_REPORTED]) if wrong else f"{len(names)} record(s)",
    )

    reported = port_version(root)
    recorded_version = tuple(version) if version is not None else None
    report.add(
        group,
        "the recorded version is the one of the headers",
        recorded_version is not None and recorded_version == reported,
        f"the record says {recorded_version}, kernel/port/info.h says {reported}",
    )

    # The record of the strict check: the calls a machine of its own has to
    # implement. The weak ones are the floor of the contract and the port may
    # leave them to the kernel.
    if optional:
        report.add(
            group,
            "the contract has a weak floor (the cores of the machine)",
            True,
            f"{len(optional)} call(s) may be left to the kernel: " + ", ".join(sorted(optional)),
        )

    return [name for name in recorded if name not in optional]


def check_independent_ports(root: str, report: Report, names: list) -> None:
    """The two machines of their own implement the whole recorded contract."""
    group = "port"
    missing = []

    if not names:
        report.skip(group, "the two independent ports implement the contract", "no recording")
        return

    for directory in PORT_INDEPENDENT_DIRS:
        absent, where = port_missing(root, directory, names)

        if absent is None:
            missing.append(f"{directory}: {where}")
        elif absent:
            shown = ", ".join(absent[:MAX_REPORTED])
            missing.append(f"{directory} ({where}): {shown}")

    report.add(
        group,
        "the two independent ports implement the contract (ESP32, x86-64)",
        not missing,
        "; ".join(missing) if missing else f"{len(names)} function(s) on both machines",
    )


IMAGE_BUDGET_BYTES = 1228800


def check_budget(root: str, report: Report) -> None:
    """The image of the kernel stays inside its budget."""
    group = "budget"
    images = []

    for entry in sorted(os.listdir(root)):
        if not entry.startswith("build"):
            continue
        candidate = os.path.join(root, entry, "micro_os.bin")
        if os.path.isfile(candidate):
            images.append(candidate)

    if not images:
        report.skip(group, "the image fits the budget", "no build*/micro_os.bin is there")
        return

    over = [
        f"{os.path.basename(os.path.dirname(path))}: {os.path.getsize(path)} bytes"
        for path in images
        if os.path.getsize(path) > IMAGE_BUDGET_BYTES
    ]
    sizes = ", ".join(
        f"{os.path.basename(os.path.dirname(path))} {os.path.getsize(path)}" for path in images
    )

    report.add(
        group,
        "the image fits the budget",
        not over,
        "; ".join(over) + f" (budget {IMAGE_BUDGET_BYTES})" if over else f"{sizes} of {IMAGE_BUDGET_BYTES}",
    )


def sram_module():
    """The budget of the static DRAM lives in a tool of its own."""
    import importlib.util

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sram_budget.py")
    spec = importlib.util.spec_from_file_location("microos_sram_budget", path)
    module = importlib.util.module_from_spec(spec)
    # The dataclass of the tool looks itself up in sys.modules by name.
    sys.modules[spec.name] = module

    # A check of the repository does not leave a __pycache__ in the tree.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous

    return module


def check_sram(root: str, report: Report) -> None:
    """The static DRAM of the board (data and bss) stays inside its budget."""
    group = "budget"
    name = "the static DRAM fits the budget"

    try:
        module = sram_module()
        measurements = module.collect_measurements(root, [], [])
    except Exception as exc:  # noqa: BLE001 - a build nobody can read is a failed check
        report.add(group, name, False, f"cannot measure the static DRAM: {exc}")
        return

    if not measurements:
        report.skip(
            group,
            name,
            "no ESP-IDF build (build*/micro_os.map or build*/idf_size.json) is there",
        )
        return

    over = [item for item in measurements if item.used > module.SRAM_BUDGET_BYTES]
    detail = "; ".join(module.describe(item) for item in measurements)
    if over:
        detail += f" (budget {module.SRAM_BUDGET_BYTES})"

    report.add(group, name, not over, detail)


# The names a feature leaves in the map when its sources are linked. A flag
# that is off must leave none of them: the component was not compiled, not
# merely not called (ADR 0010).
COMPONENT_FEATURES = (
    ("MICRO_OS_NET", ("net_register",)),
    ("MICRO_OS_NET_HTTP", ("http_get",)),
    ("MICRO_OS_NET_HTTP_SERVER", ("net_http_server_start",)),
    ("MICRO_OS_NET_NTP", ("ntp_parse_response",)),
    ("MICRO_OS_NET_MQTT", ("mqtt_connect", "mqtt_publish")),
    ("MICRO_OS_TELNET", ("telnet_register",)),
    ("MICRO_OS_WIFI", ("wifi_init",)),
)

# The kernel flags are compile definitions, not Kconfig symbols: the value is
# read from the flags of the kernel sources in compile_commands.json.
KERNEL_FEATURES = (
    ("KERNEL_PSRAM", ("kmm_psram",)),
    ("KERNEL_EXEC", ("k_exec_parse", "k_exec_run")),
)


def build_definitions(build_dir: str) -> dict:
    """The -D flags of the kernel sources of one build, from its commands."""
    path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.isfile(path):
        return {}

    try:
        with open(path, "r", encoding="utf-8") as handle:
            entries = json.load(handle)
    except (OSError, ValueError):
        return {}

    definitions = {}
    for entry in entries:
        source = str(entry.get("file", "")).replace("\\", "/")
        if "/components/kernel/src/" not in source:
            continue

        arguments = entry.get("arguments")
        if arguments is None:
            try:
                arguments = shlex.split(entry.get("command", ""))
            except ValueError:
                continue

        for argument in arguments:
            if argument.startswith("-D") and "=" in argument:
                name, value = argument[2:].split("=", 1)
                try:
                    value = int(value)
                except ValueError:
                    pass
                definitions.setdefault(name, value)

    return definitions


def feature_values(build_dir: str) -> dict:
    """The flags of one build: the component ones from sdkconfig, the rest.

    ESP-IDF writes the enabled CONFIG_ symbols to build/config/sdkconfig.h
    and leaves a disabled one out entirely, so a known component flag that
    is not defined there was off in that build.
    """
    values = {}

    path = os.path.join(build_dir, "config", "sdkconfig.h")
    if os.path.isfile(path):
        text = read_text(path)
        for name, _ in COMPONENT_FEATURES:
            pattern = r"^#define\s+CONFIG_" + re.escape(name) + r"\s+1\b"
            values[name] = 1 if re.search(pattern, text, re.MULTILINE) else 0

    values.update(build_definitions(build_dir))

    return values


def check_features(root: str, report: Report) -> None:
    """A disabled feature leaves no symbol of itself in the image map.

    The check needs a build: without one it skips and never fails a checkout
    that has not been compiled yet. With a build it compares the flags of
    that build with the names in its map - the mistake it catches is a
    feature that the build system says is off while its object is still in
    the image.
    """
    group = "features"
    maps = []

    for entry in sorted(os.listdir(root)):
        if not entry.startswith("build"):
            continue
        candidate = os.path.join(root, entry, "micro_os.map")
        if os.path.isfile(candidate):
            maps.append(candidate)

    if not maps:
        report.skip(group, "the disabled features are absent from the map", "no build*/micro_os.map")
        return

    for path in maps:
        build_dir = os.path.dirname(path)
        name = os.path.basename(build_dir)
        values = feature_values(build_dir)
        map_text = read_text(path)
        checked = 0

        for flag, symbols in COMPONENT_FEATURES + KERNEL_FEATURES:
            if values.get(flag) != 0:
                continue

            present = [symbol for symbol in symbols if symbol in map_text]
            report.add(
                group,
                f"{name}: {flag}=0 leaves no {'/'.join(symbols)}",
                not present,
                "still in the map: " + ", ".join(present) if present else "absent",
            )
            checked += 1

        if checked == 0:
            report.skip(
                group,
                f"{name}: the map agrees with the flags",
                "no disabled feature to check",
            )

    # The host build has no map, but a component that is off leaves no
    # library either: that is the cheapest proof that CMake did not compile
    # its sources (make test-minimal).
    for entry in sorted(os.listdir(root)):
        cache_path = os.path.join(root, entry, "CMakeCache.txt")
        if not entry.startswith("build") or not os.path.isfile(cache_path):
            continue

        cache = parse_sdkconfig(cache_path)
        for flag, component, library in (
            ("MICRO_OS_NET", "net", "microos_net"),
            ("MICRO_OS_TELNET", "telnet", "microos_telnet"),
            ("MICRO_OS_WIFI", "wifi", "microos_wifi"),
        ):
            if cache.get(flag + ":BOOL") != "OFF":
                continue

            path = os.path.join(root, entry, "components", component, "lib" + library + ".a")
            report.add(
                group,
                f"{entry}: {flag}=OFF leaves no lib{library}.a",
                not os.path.isfile(path),
                "the library is still there" if os.path.isfile(path) else "absent",
            )


def check_version(root: str, report: Report, expected: str) -> None:
    """The version of the kernel against the expected one (the tag)."""
    group = "version"
    path = os.path.join(root, "components", "kernel", "include", "kernel", "config.h")

    if not os.path.isfile(path):
        report.add(group, "the version of the kernel is known", False, "config.h is missing")
        return

    text = read_text(path)
    parts = []
    for key in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(r"#define\s+KERNEL_VERSION_%s\s+(\d+)" % key, text)
        if match is None:
            report.add(
                group,
                "the version of the kernel is known",
                False,
                f"KERNEL_VERSION_{key} is not defined",
            )
            return
        parts.append(match.group(1))

    version = ".".join(parts)
    report.add(
        group,
        "the version of the kernel is known",
        True,
        version,
    )
    report.add(
        group,
        f"the version of the kernel is {expected}",
        version == expected,
        "" if version == expected else f"the kernel says {version}, expected {expected}",
    )


# --- the entry point ---------------------------------------------------------


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the integrity check of Micro OS")
    parser.add_argument(
        "--root",
        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        help="the root of the repository",
    )
    parser.add_argument("--skip-format", action="store_true", help="do not run clang-format")
    parser.add_argument("--skip-git", action="store_true", help="do not run git fsck")
    parser.add_argument(
        "--expect-version",
        metavar="X.Y.Z",
        help="the version the kernel must have (the version of the tag)",
    )
    parser.add_argument("--json", metavar="FILE", help="write the report as JSON")
    parser.add_argument("-v", "--verbose", action="store_true", help="print the passed checks too")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    report = Report()

    files = tracked_files(root)
    if not files:
        report.skip("sources", "the file list is known", "git ls-files is not available")

    check_bundle(root, report)
    check_artifacts(root, report)
    images = check_images(root, report)
    check_progs(root, report)
    table = check_partitions(root, report)
    check_config(root, report, images, table)
    check_layers(root, report, files)
    check_portable_build(root, report)
    check_port_contract(root, report)
    recorded_contract = check_port_recording(root, report)
    check_independent_ports(root, report, recorded_contract)
    check_budget(root, report)
    check_sram(root, report)
    check_features(root, report)
    if args.expect_version:
        check_version(root, report, args.expect_version)
    if not args.skip_git:
        check_git(root, report)
    check_sources(root, report, files)
    if not args.skip_format:
        check_format(root, report, files, "clang-format")

    failed = report.failed

    print("Micro OS — the integrity check")
    print("=" * 60)
    groups = []
    for check in report.checks:
        if check["group"] not in groups:
            groups.append(check["group"])

    for group in groups:
        print(f"\n[{group}]")
        for check in report.checks:
            if check["group"] != group:
                continue
            if check.get("skipped"):
                mark = "skip"
            elif check["ok"]:
                mark = " ok "
            else:
                mark = "FAIL"
            print(f"  {mark}  {check['name']}")
            if check["detail"] and (not check["ok"] or args.verbose):
                print(f"          {check['detail']}")

    print("\n" + "=" * 60)
    if failed:
        print(f"FAILED: {len(failed)} of {len(report.checks)} check(s) did not pass")
    else:
        print(f"OK: {report.passed} check(s) passed")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "ok": not failed,
                    "passed": report.passed,
                    "failed": len(failed),
                    "checks": report.checks,
                },
                handle,
                indent=2,
                ensure_ascii=False,
            )
            handle.write("\n")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
