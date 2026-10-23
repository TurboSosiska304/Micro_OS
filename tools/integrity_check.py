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

The exit status is 0 when everything is in order and 1 otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
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
BUNDLE_BINARIES = ("micro_os.bin", "bootloader.bin", "partition-table.bin")
BUNDLE_EXTRA = ("checksums.txt", "flash.sh", "README.md")

# The extensions that are never treated as a text file.
BINARY_EXTENSIONS = {
    ".bin",
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
    """The files of the repository.

    The list comes from git, so that the ignored files (the build
    directories) are not checked. When git is not available the tree is
    walked instead and the obvious build directories are skipped.
    """
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"],
            capture_output=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return walk_files(root)

    names = [name for name in out.stdout.decode("utf-8", "replace").split("\0") if name]
    return names if names else walk_files(root)


# The directories that never hold the sources of the project.
SKIPPED_DIRS = {".git", "build", "build-host", "managed_components", "__pycache__"}


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

        report.add(
            group,
            "checksums.txt covers every binary",
            set(sums) == set(BUNDLE_BINARIES),
            f"expected {sorted(BUNDLE_BINARIES)}, got {sorted(sums)}",
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


def check_git(root: str, report: Report) -> None:
    """The integrity of the git objects."""
    group = "git"

    if not os.path.isdir(os.path.join(root, ".git")):
        report.skip(group, "git fsck", "the directory is not a git repository")
        return

    try:
        result = subprocess.run(
            ["git", "-C", root, "fsck", "--strict", "--no-progress"],
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
    images = check_images(root, report)
    table = check_partitions(root, report)
    check_config(root, report, images, table)
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
