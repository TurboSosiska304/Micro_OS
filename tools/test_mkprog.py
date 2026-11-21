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
visible. One case goes one step further and links an object for a window with
program.ld's rule (the text at the base plus the header), then asks the script
to check that window: the right base has to pass, and a base the object was
not linked for has to be refused instead of producing an image that runs at
the wrong addresses.

The field `arch` (offset 28, version 2 of the format) is checked the same
way: the default is x86-64, both spellings of a machine write its number, and
a machine the format does not know is refused before an image exists. The
rest of the header stays where it was, so the test of the layout is also the
test that a new field did not move the others. A port whose data is a window
of its own - the board - passes --data-base: the test links an object with
the data at another address and checks that the script holds it there and
refuses a window the object was not linked for. It needs no ESP-IDF and no
board, only the binutils that mkprog.sh itself uses:

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
K_EXEC_VERSION = 2

# The numbers of the machine field, the header of kernel/exec.h.
K_EXEC_ARCH_X86_64 = 0
K_EXEC_ARCH_XTENSA = 1

# The name of the section of the header, in the order of the fields.
HEADER_FIELDS = (
    "magic",
    "version",
    "flags",
    "entry",
    "text_size",
    "data_size",
    "bss_size",
    "arch",
)

# The lengths of the segments the test builds: every remainder of a division
# by four, and a couple of longer segments. (text, data).
CASES = ((1, 0), (3, 3), (5, 1), (7, 4), (9, 2))

# A window for the case of the base check, the kind a port names. The number
# is not important; what is important is that the object is linked for it and
# that a different one is refused.
PROGRAM_BASE = 0x02000000

# The two windows of the board, for the case of --data-base: they are not
# contiguous, and the script has to check both.
PROGRAM_TEXT_BASE = 0x02000000
PROGRAM_DATA_BASE = 0x03000000


def build_linker_script(base: int) -> str:
    """The rule of program.ld: the text at base + 32, the data after it."""
    return f"""\
SECTIONS
{{
    . = 0x{base + K_EXEC_HEADER_SIZE:08x};
    .text : SUBALIGN(4) {{ *(.text .text.*) }}
    . = ALIGN(4);
    .data : {{ *(.data .data.*) *(.rodata .rodata.*) }}
    /DISCARD/ : {{ *(.comment) *(.eh_frame) *(.note .note.*) }}
}}
"""


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


def check_case(
    work: Path,
    script: Path,
    text: bytes,
    data: bytes,
    arch_args: tuple = (),
    arch_number: int = K_EXEC_ARCH_X86_64,
) -> None:
    """One object through the script, against the layout of the header."""
    name = f"text{len(text)}-data{len(data)}-arch{arch_number}"
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
        [str(script), *arch_args, str(obj), str(image), "test_entry"],
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
    assert fields["arch"] == arch_number, (
        f"{name}: arch is {fields['arch']}, expected {arch_number}"
    )
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


def check_base_case(work: Path, script: Path) -> None:
    """An object linked for a window: the right base passes, another refuses."""
    text = bytes(0x90 + index % 8 for index in range(5))
    data = bytes(0x40 + 3 * index for index in range(3))

    source = work / "base.c"
    obj = work / "base.o"
    linker = work / "base.ld"
    elf = work / "base.elf"

    source.write_text(build_source(text, data), encoding="utf-8")
    linker.write_text(build_linker_script(PROGRAM_BASE), encoding="utf-8")
    subprocess.run(
        ["gcc", "-c", "-fno-pic", "-fno-pie", "-fno-stack-protector",
         "-fno-asynchronous-unwind-tables", "-o", str(obj), str(source)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["gcc", "-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
         "-Wl,-e,test_entry", f"-Wl,-T,{linker}", "-o", str(elf), str(obj)],
        check=True,
        capture_output=True,
        text=True,
    )

    # The base the linker used is the one the script is given: a success, and
    # the image is still laid out the way the header says.
    image = work / "base.mox"
    subprocess.run(
        [str(script), "--base", hex(PROGRAM_BASE), "--arch", "x86-64", str(elf), str(image),
         "test_entry"],
        check=True,
        capture_output=True,
        text=True,
    )
    header, payload = parse_image(image)
    fields = dict(zip(HEADER_FIELDS, header))
    assert fields["arch"] == K_EXEC_ARCH_X86_64, f"base: arch is {fields['arch']}"
    assert fields["entry"] == K_EXEC_HEADER_SIZE, f"base: entry is {fields['entry']}"
    assert fields["text_size"] == align4(len(text)), (
        f"base: text size is {fields['text_size']}"
    )
    assert fields["data_size"] == align4(len(data)), (
        f"base: data size is {fields['data_size']}"
    )
    assert payload[: len(text)] == text, "base: the text bytes moved"
    assert payload[align4(len(text)) : align4(len(text)) + len(data)] == data, (
        "base: the data is not after the padded text"
    )

    # A different window means the image was linked for addresses this port
    # will not give it. The script has to refuse and not write the image.
    wrong = PROGRAM_BASE + 0x00100000
    refused = work / "refused.mox"
    result = subprocess.run(
        [str(script), "--base", hex(wrong), str(elf), str(refused), "test_entry"],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0, "base: a wrong window was accepted"
    assert "is linked for" in result.stderr, (
        f"base: the refusal does not say what the object was linked for: {result.stderr!r}"
    )
    assert "the window starts at" in result.stderr, (
        f"base: the refusal does not say where the window starts: {result.stderr!r}"
    )
    assert not refused.exists(), "base: the refusal left an image behind"


def build_split_linker_script(text_base: int, data_base: int) -> str:
    """The rule of program_esp32.ld: the text above, the data in its own window."""
    return f"""\
SECTIONS
{{
    . = 0x{text_base + K_EXEC_HEADER_SIZE:08x};
    .text : SUBALIGN(4) {{ *(.text .text.*) }}
    . = 0x{data_base:08x};
    .data : SUBALIGN(4) {{ *(.data .data.*) *(.rodata .rodata.*) }}
    . = ALIGN(4);
    .bss (NOLOAD) : SUBALIGN(4) {{ *(.bss .bss.*) *(COMMON) }}
    /DISCARD/ : {{ *(.comment) *(.eh_frame) *(.note .note.*) }}
}}
"""


def check_data_base_case(work: Path, script: Path) -> None:
    """A split image: the data is in its own window and the script holds it there."""
    text = bytes(0x90 + index % 8 for index in range(5))
    data = bytes(0x40 + 3 * index for index in range(3))

    source = work / "split.c"
    obj = work / "split.o"
    linker = work / "split.ld"
    elf = work / "split.elf"

    # A bss of four words: the image does not carry it, and the loader clears
    # it at the address the script gives it. The test checks that address too.
    source.write_text(
        build_source(text, data) + "\nint test_bss[4];\n", encoding="utf-8"
    )
    linker.write_text(
        build_split_linker_script(PROGRAM_TEXT_BASE, PROGRAM_DATA_BASE), encoding="utf-8"
    )
    subprocess.run(
        ["gcc", "-c", "-fno-pic", "-fno-pie", "-fno-stack-protector",
         "-fno-asynchronous-unwind-tables", "-o", str(obj), str(source)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["gcc", "-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
         "-Wl,-e,test_entry", f"-Wl,-T,{linker}", "-o", str(elf), str(obj)],
        check=True,
        capture_output=True,
        text=True,
    )

    image = work / "split.mox"
    subprocess.run(
        [str(script), "--base", hex(PROGRAM_TEXT_BASE), "--data-base", hex(PROGRAM_DATA_BASE),
         "--arch", "xtensa", str(elf), str(image), "test_entry"],
        check=True,
        capture_output=True,
        text=True,
    )

    header, payload = parse_image(image)
    fields = dict(zip(HEADER_FIELDS, header))
    assert fields["arch"] == K_EXEC_ARCH_XTENSA, f"split: arch is {fields['arch']}"
    assert fields["entry"] == K_EXEC_HEADER_SIZE, f"split: entry is {fields['entry']}"
    assert fields["text_size"] == align4(len(text)), f"split: text size is {fields['text_size']}"
    assert fields["data_size"] == align4(len(data)), f"split: data size is {fields['data_size']}"
    assert fields["bss_size"] == 16, f"split: bss size is {fields['bss_size']}"
    assert payload[: len(text)] == text, "split: the text bytes moved"
    assert payload[align4(len(text)) : align4(len(text)) + len(data)] == data, (
        "split: the data is not after the padded text in the file"
    )

    # The data window the object was not linked for is refused, and so is the
    # text window: an image at the wrong addresses is a fault, not a program.
    for option, wrong, message in (
        ("--data-base", PROGRAM_DATA_BASE + 0x00100000, "the data window starts at"),
        ("--base", PROGRAM_TEXT_BASE + 0x00100000, "is linked for"),
    ):
        if option == "--base":
            args = ["--base", hex(wrong), "--data-base", hex(PROGRAM_DATA_BASE)]
        else:
            args = ["--base", hex(PROGRAM_TEXT_BASE), "--data-base", hex(wrong)]

        refused = work / f"refused{option}.mox"
        result = subprocess.run(
            [str(script), *args, "--arch", "xtensa", str(elf), str(refused), "test_entry"],
            capture_output=True,
            text=True,
        )
        assert result.returncode != 0, f"split: a wrong {option} was accepted"
        assert message in result.stderr, (
            f"split: the refusal of {option} does not say why: {result.stderr!r}"
        )
        assert not refused.exists(), f"split: the refusal of {option} left an image behind"


def check_bad_arch(work: Path, script: Path) -> None:
    """A machine the format does not know is refused, and no image is left."""
    text = bytes(0x90 + index % 8 for index in range(5))

    source = work / "badarch.c"
    obj = work / "badarch.o"
    image = work / "badarch.mox"

    source.write_text(build_source(text, b""), encoding="utf-8")
    subprocess.run(
        ["gcc", "-c", "-fno-pic", "-fno-stack-protector", "-o", str(obj), str(source)],
        check=True,
        capture_output=True,
        text=True,
    )

    # The names of other machines, a name of the wrong case, an empty one and
    # a number: none of them is a machine of the format, and the refusal says
    # what the script accepts.
    for name in ("mips", "arm", "X86-64", "x86_64", ""):
        if image.exists():
            image.unlink()

        result = subprocess.run(
            [str(script), "--arch", name, str(obj), str(image), "test_entry"],
            capture_output=True,
            text=True,
        )
        assert result.returncode != 0, f"arch: '{name}' was accepted"
        assert "--arch wants x86-64 or xtensa" in result.stderr, (
            f"arch: the refusal of '{name}' does not say what is accepted: {result.stderr!r}"
        )
        assert not image.exists(), f"arch: the refusal of '{name}' left an image behind"


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
            # the text looks like code, the data does not. No --arch is given:
            # the x86-64 port builds its images this way, so the default is a
            # promise of the script.
            text = bytes(0x90 + index % 8 for index in range(text_size))
            data = bytes(0x40 + 3 * index for index in range(data_size))
            check_case(work, script, text, data)

        # Both spellings of the option: the machine of the board, and the
        # machine of the port written out by name.
        text = bytes(0x90 + index % 8 for index in range(5))
        data = bytes(0x40 + 3 * index for index in range(1))
        check_case(
            work, script, text, data, arch_args=("--arch", "xtensa"),
            arch_number=K_EXEC_ARCH_XTENSA,
        )
        check_case(
            work, script, text, data, arch_args=("--arch=x86-64",),
            arch_number=K_EXEC_ARCH_X86_64,
        )

        check_bad_arch(work, script)
        check_base_case(work, script)
        check_data_base_case(work, script)

    print(
        f"test_mkprog: OK, {len(CASES)} image(s) laid out as the header says, "
        "both machines written, the base of the window and the data window checked"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f"test_mkprog: FAIL: {error}")
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr)
        sys.exit(1)
