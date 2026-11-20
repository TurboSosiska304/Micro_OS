#!/usr/bin/env python3
"""Micro OS — the test of tools/mkprog.sh.

The script turns a relocatable object into the flat MOX1 image. The layout is
easy to get subtly wrong: the data begins at the header plus the *padded*
text, and a pad written once at the end of the file leaves the data one, two
or three bytes short of the address the header names. That bug ran, printed
the wrong bytes and looked like a fault in the program, so the padding of
every segment is checked here and not by eye.

The test builds a few tiny objects with gcc: the .text and the .data are not
multiples of four on purpose, which is the only case where the padding is
visible. It needs no ESP-IDF and no board, only the binutils that mkprog.sh
itself uses:

    python3 tools/test_mkprog.py
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

K_EXEC_HEADER_SIZE = 32
K_EXEC_MAGIC = 0x31584F4D
K_EXEC_VERSION = 1

# The name of the section of the header, in the order of the fields.
HEADER_FIELDS = (
    "magic",
    "version",
    "flags",
    "entry",
    "text_size",
    "data_size",
    "bss_size",
    "reserved",
)

# The lengths of the segments the test builds: every remainder of a division
# by four, and a couple of longer segments. (text, data).
CASES = ((1, 0), (3, 3), (5, 1), (7, 4), (9, 2))


def align4(value: int) -> int:
    """Round a size up to four bytes, the way the format and the script do."""
    return (value + 3) // 4 * 4


def build_source(text: bytes, data: bytes) -> str:
    """A C file whose .text and .data have exactly the bytes asked for."""
    lines = [
        "__asm__(",
        '    ".text\\n"',
        '    ".globl test_entry\\n"',
        '    ".type test_entry, @function\\n"',
        '    "test_entry:\\n"',
        '    ".byte ' + ", ".join(f"0x{byte:02x}" for byte in text) + '\\n"',
        f'    ".size test_entry, {len(text)}\\n"',
    ]

    if data:
        lines += [
            '    ".section .data\\n"',
            '    ".globl test_payload\\n"',
            '    ".type test_payload, @object\\n"',
            '    "test_payload:\\n"',
            '    ".byte ' + ", ".join(f"0x{byte:02x}" for byte in data) + '\\n"',
            f'    ".size test_payload, {len(data)}\\n"',
        ]

    lines += [");", ""]
    return "\n".join(lines)


def parse_image(path: Path) -> tuple:
    """The header and the bytes of an image, with the header as a tuple."""
    raw = path.read_bytes()
    if len(raw) < K_EXEC_HEADER_SIZE:
        raise AssertionError(f"{path}: the image is shorter than the header ({len(raw)} bytes)")

    header = struct.unpack("<8I", raw[:K_EXEC_HEADER_SIZE])
    return header, raw[K_EXEC_HEADER_SIZE:]


def check_case(work: Path, script: Path, text: bytes, data: bytes) -> None:
    """One object through the script, against the layout of the header."""
    name = f"text{len(text)}-data{len(data)}"
    source = work / f"{name}.c"
    obj = work / f"{name}.o"
    image = work / f"{name}.mox"

    source.write_text(build_source(text, data), encoding="utf-8")
    subprocess.run(
        ["gcc", "-c", "-fno-pic", "-fno-stack-protector", "-o", str(obj), str(source)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        [str(script), str(obj), str(image), "test_entry"],
        check=True,
        capture_output=True,
        text=True,
    )

    header, payload = parse_image(image)
    fields = dict(zip(HEADER_FIELDS, header))

    text_aligned = align4(len(text))
    data_aligned = align4(len(data))

    assert fields["magic"] == K_EXEC_MAGIC, f"{name}: magic is 0x{fields['magic']:08X}"
    assert fields["version"] == K_EXEC_VERSION, f"{name}: version is {fields['version']}"
    assert fields["flags"] == 0, f"{name}: flags are 0x{fields['flags']:08X}"
    assert fields["reserved"] == 0, f"{name}: reserved is 0x{fields['reserved']:08X}"
    assert fields["bss_size"] == 0, f"{name}: bss size is {fields['bss_size']}"

    # The entry is an offset from the start of the image, header included, and
    # the symbol sits at the start of the text of the object.
    assert fields["entry"] == K_EXEC_HEADER_SIZE, f"{name}: entry is {fields['entry']}"

    # The header rounds the sizes up; the file has to carry that padding, or
    # the port tears the last word of a segment.
    assert fields["text_size"] == text_aligned, (
        f"{name}: text size is {fields['text_size']}, expected {text_aligned}"
    )
    assert fields["data_size"] == data_aligned, (
        f"{name}: data size is {fields['data_size']}, expected {data_aligned}"
    )

    # The bug this test exists for: the data of the file starts at the header
    # plus the aligned text. A single pad at the end of the file shifts it by
    # the padding of the text and the header stops describing the bytes.
    expected_size = K_EXEC_HEADER_SIZE + text_aligned + data_aligned
    assert len(payload) == text_aligned + data_aligned, (
        f"{name}: the image has {len(payload)} payload bytes, expected "
        f"{text_aligned + data_aligned} (the file is {len(payload) + K_EXEC_HEADER_SIZE})"
    )
    assert len(payload) + K_EXEC_HEADER_SIZE == expected_size, f"{name}: the image size is wrong"

    assert payload[: len(text)] == text, f"{name}: the text bytes moved"
    assert payload[len(text) : text_aligned] == b"\0" * (text_aligned - len(text)), (
        f"{name}: the padding of the text is not zero"
    )
    assert payload[text_aligned : text_aligned + len(data)] == data, (
        f"{name}: the data is not at 32 + align4(text) = {K_EXEC_HEADER_SIZE + text_aligned}"
    )
    assert payload[text_aligned + len(data) :] == b"\0" * (data_aligned - len(data)), (
        f"{name}: the padding of the data is not zero"
    )


def main() -> int:
    tools = ("gcc", "nm", "objcopy", "objdump")
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        print(f"test_mkprog: skip, not found: {', '.join(missing)}")
        return 0

    root = Path(__file__).resolve().parent.parent
    script = root / "tools" / "mkprog.sh"

    if not script.is_file():
        print(f"test_mkprog: the script is missing: {script}")
        return 1

    with tempfile.TemporaryDirectory(prefix="microos-mkprog-") as directory:
        work = Path(directory)
        for text_size, data_size in CASES:
            # The bytes are not a pattern the script could reproduce by luck:
            # the text looks like code, the data does not.
            text = bytes(0x90 + index % 8 for index in range(text_size))
            data = bytes(0x40 + 3 * index for index in range(data_size))
            check_case(work, script, text, data)

    print(f"test_mkprog: OK, {len(CASES)} image(s) laid out as the header says")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f"test_mkprog: FAIL: {error}")
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr)
        sys.exit(1)
