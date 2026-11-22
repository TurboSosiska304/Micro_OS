#!/usr/bin/env python3
"""Micro OS — the image of the disk of the x86-64 port.

The machine has no programs in its kernel any more: `/bin` and `/etc` are
objects of a MicroFS volume on the disk, and the boot mounts that volume and
runs `/bin/hello` through `execve`. This script calls the host tool that
writes the volume (`tools/mkfs_mfs.c`, which links `components/mfs`), so the
format has one implementation and not a second one in Python.

The image is a whole disk: an MBR, one Linux partition at LBA 2048 and a
MicroFS volume in it, with the `.mox` images of the build in `/bin`, an
empty `/etc/init.d` and the settings of the machine in `/etc/setting`.

    tools/x86_image.py [--image PATH] [--build DIR] [--mkfs PATH]
                       [--size-mb N] [--source-date-epoch N] [--verbose]

The exit status is 0 when the image was written, 2 when the tool or one of
the images of a program is missing, and 1 when the tool failed.
"""

from __future__ import annotations

import argparse
import gzip
import io
import os
import subprocess
import sys

# The programs of /bin, in the order of the list of the port. The name is
# the file in the build directory and the file of the volume.
PROGRAMS = [
    "hello",
    "crash",
    "fork",
    "badptr",
    "signals",
    "cat",
    "echo",
    "ls",
    "wc",
    "head",
    "grep",
    "mkdir",
    "rm",
    "pwd",
    "spawn",
    "nx",
    "zcat",
    "pcre2grep",
    "lua",
    # The tools of sbase (third_party/sbase) are the commands of the machine
    # under their own names: cat, grep, head, ls and wc replace the small
    # programs of ADR 0016, and tail, sort, find, xargs and sed are new.
    "tail",
    "sort",
    "find",
    "xargs",
    "sed",
    # The two calculators of the third port: one engine, two names.
    "bc",
    "dc",
]

# The stream of the test of zlib: a fixed text, a fixed moment and the level
# of compression of the tool. The smoke test compares the bytes `zcat` writes
# with this text, so the file is a fixture of the machine and not a sample
# that changes with the build of the day.
GREETING = (
    b"Micro OS: the first port of a program\n"
    b"zlib 1.3.1, MicroFS and ring 3\n"
    b"the bytes of this file were inflated by the library of the guest\n"
)

# The license of PCRE2 travels with the machine that carries its program: the
# text of third_party/pcre2/LICENSE is a file of /etc/licenses, so a person
# with the disk can read the terms of the one third-party component of the
# image without the sources.
PCRE2_LICENSE = "third_party/pcre2/LICENSE"

# The one file of /etc the smoke test reads. The tool writes the text and a
# newline; the first line is the word `cat`, `grep` and `wc` expect, the two
# after it are the settings a machine of this port has (the config component
# is not linked here, so the file is data of the image).
SETTING = "hello\nhostname=x86_64\nconsole=vga"

# The first script of the machine: a table, a number with a point in it, a
# file of the volume written and read back, and the clock. The text is a
# fixture like the greeting of zlib: the smoke test knows every line of it and
# the two runs of a reproducible build write the same bytes.
HELLO_LUA = """-- /etc/hello.lua: the first script of the machine
local kinds = { "alpha", "beta", "gamma" }
print("lua: hello from /etc/hello.lua")
print("lua: table", #kinds, kinds[1], kinds[3])
print(string.format("lua: pi %.3f, 2^10 %.0f", math.pi, 2 ^ 10))
print("lua: sqrt", math.sqrt(2), "floor", math.floor(-3.5))
local out = assert(io.open("/hello.txt", "w"))
out:write("a line written by lua\\n")
out:close()
local input = assert(io.open("/hello.txt", "r"))
print("lua: read", input:read("l"))
input:close()
os.remove("/hello.txt")
print("lua: os.time", os.time() >= 0)
print("lua: string", ("abc"):upper(), string.rep("x", 3))
"""

# The script of bc: the four things a calculator of the machine has to show -
# a number of 31 digits, a square root with the scale of the line, a base read
# and a base written, a function of the language with a loop and an array, a
# string, and the math library of `-l`. The smoke test reads this text in one
# run and again after the reset of the disk. The last line is the arctangent
# of 1 at `scale=10`: the math library truncates the constant of the release
# at the scale of the call, as GNU bc does, so the tenth digit is 2 and not
# the 5 of a value rounded at the last place.
BC_SCRIPT = """/* /etc/bc.bc: the script of the machine */
scale = 20
2^100
sqrt(2)
ibase = 16
FF
ibase = A
obase = 2
10
obase = A
define sum(n) {
    auto i, s
    s = 0
    for (i = 1; i <= n; i++) s += i
    return (s)
}
sum(10)
if (sum(10) == 55) print "the sum is right\\n"
i = 0
while (i < 3) i += 1
i
a[1] = 10
a[2] = 32
a[1] + a[2]
scale = 10
4*a(1)
"""

# The script of dc: the same engine in RPN. The lines are a sum, a division
# at the default scale of 0, a division at a scale of 5 and a power.
DC_SCRIPT = """2 3 + p
10 4 / p
5 k 1 3 / p
2 10 ^ p
"""

IMAGE_MB = 8
START_LBA = 2048
# The moment of the records when SOURCE_DATE_EPOCH does not say another one.
# Zero is the convention of a reproducible build and "unknown" in MicroFS:
# an image is not made by a clock, and two runs have to give the same bytes.
SOURCE_DATE_EPOCH = 0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
ZLIB_LICENSE = os.path.join(ROOT, "third_party", "zlib", "LICENSE")
LUA_LICENSE = os.path.join(ROOT, "third_party", "lua", "LICENSE")
SBASE_LICENSE = os.path.join(ROOT, "third_party", "sbase", "LICENSE")
BC_LICENSE = os.path.join(ROOT, "third_party", "bc", "LICENSE")


def write_text_fixture(build_dir: str, name: str, text: str) -> str:
    """Write a script of the machine, the same bytes every run."""
    path = os.path.join(build_dir, name)

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)

    return path


def write_bc_script(build_dir: str) -> str:
    return write_text_fixture(build_dir, "bc.bc", BC_SCRIPT)


def write_dc_script(build_dir: str) -> str:
    return write_text_fixture(build_dir, "dc.dc", DC_SCRIPT)


def write_hello_lua(build_dir: str) -> str:
    """Write the script of the first Lua program, the same bytes every run."""
    path = os.path.join(build_dir, "hello.lua")

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(HELLO_LUA)

    return path


def write_greeting(build_dir: str, epoch: int) -> str:
    """Write the .gz of the fixture, the same bytes for the same inputs.

    The mtime of the gzip header is SOURCE_DATE_EPOCH, the name of the stream
    is empty (a name would be a fact of the host path), and the level is the
    one of the library. Two runs of the image tool then write the same file,
    which is what `make reproducible` compares.
    """
    path = os.path.join(build_dir, "greeting.gz")
    buffer = io.BytesIO()

    with gzip.GzipFile(
        filename="",
        mode="wb",
        fileobj=buffer,
        mtime=epoch,
        compresslevel=9,
    ) as stream:
        stream.write(GREETING)

    with open(path, "wb") as file:
        file.write(buffer.getvalue())

    return path


def build_image(
    image: str,
    build_dir: str = DEFAULT_BUILD,
    mkfs: str = DEFAULT_MKFS,
    size_mb: int = IMAGE_MB,
    epoch: int = SOURCE_DATE_EPOCH,
    verbose: bool = False,
    extra: list | None = None,
) -> int:
    """Write the image with the programs of the build and /etc/setting.

    `extra` is a list of the arguments of the tool that a caller needs on top
    of the tree of the machine: `--file PATH=HOST`, `--text PATH=TEXT` and
    `--dir PATH`. The stress tools use it to put a filler on the volume and
    reach `ENOSPC` in a boot; the image of a person stays the default tree.
    """
    if not os.path.isfile(mkfs):
        print(f"missing {mkfs}: run make build", file=sys.stderr)
        return 2

    command = [
        mkfs,
        "--image",
        image,
        "--size-mb",
        str(size_mb),
        "--start-lba",
        str(START_LBA),
        "--source-date-epoch",
        str(epoch),
        "--dir",
        "/bin",
        "--dir",
        "/etc",
        "--dir",
        "/etc/init.d",
        "--dir",
        "/etc/licenses",
        # The mount points of the virtual filesystems: the boot mounts /proc
        # and /dev over these empty directories, and a listing of the root
        # that reads the volume (the `ls` of sbase) then finds the two names
        # as well as a listing of the shell that asks the VFS.
        "--dir",
        "/proc",
        "--dir",
        "/dev",
    ]

    for name in PROGRAMS:
        path = os.path.join(build_dir, f"{name}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        command += ["--program", f"/bin/{name}={path}"]

    if not os.path.isfile(ZLIB_LICENSE):
        print(f"missing {ZLIB_LICENSE}", file=sys.stderr)
        return 2
    if not os.path.isfile(LUA_LICENSE):
        print(f"missing {LUA_LICENSE}", file=sys.stderr)
        return 2
    if not os.path.isfile(SBASE_LICENSE):
        print(f"missing {SBASE_LICENSE}", file=sys.stderr)
        return 2
    if not os.path.isfile(BC_LICENSE):
        print(f"missing {BC_LICENSE}", file=sys.stderr)
        return 2

    # The fixture of zcat and the license that travels with its bytes. The
    # program is part of the image, so the image carries the notice of the
    # library it was linked against; a package that ships zcat copies the same
    # text beside the program.
    greeting = write_greeting(build_dir, epoch)
    command += ["--file", f"/etc/greeting.gz={greeting}"]
    command += ["--file", f"/etc/licenses/zlib={ZLIB_LICENSE}"]

    # The script of Lua and the license of its library. The script is a file
    # of the volume and not a line of a test: the program reads it through
    # the VFS and the smoke test reads the lines the program printed.
    script = write_hello_lua(build_dir)
    command += ["--file", f"/etc/hello.lua={script}"]
    command += ["--file", f"/etc/licenses/lua={LUA_LICENSE}"]

    # The MIT notice of sbase travels with the ten commands of /bin, as the
    # notice of zlib travels with zcat.
    command += ["--file", f"/etc/licenses/sbase={SBASE_LICENSE}"]

    # The two calculators: the scripts of /etc and the license of the library
    # that both images carry. bc 7.2.0 is one engine with two names, so one
    # notice of the release is beside the two programs.
    bc_script = write_bc_script(build_dir)
    dc_script = write_dc_script(build_dir)
    command += ["--file", f"/etc/bc.bc={bc_script}"]
    command += ["--file", f"/etc/dc.dc={dc_script}"]
    command += ["--file", f"/etc/licenses/bc={BC_LICENSE}"]

    command += ["--text", f"/etc/setting={SETTING}"]

    license_path = os.path.join(ROOT, PCRE2_LICENSE)
    if not os.path.isfile(license_path):
        print(f"missing {license_path}", file=sys.stderr)
        return 2
    command += ["--file", f"/etc/licenses/pcre2={license_path}"]

    command += extra or []

    os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)

    if verbose:
        print(" ".join(command))

    result = subprocess.run(command, check=False)
    return result.returncode


def parse_epoch(text: str) -> int:
    """The seconds of SOURCE_DATE_EPOCH, or a value argparse refuses."""
    try:
        value = int(text, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text}") from error
    if value < 0:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text}")
    return value


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk image of the x86-64 port")
    parser.add_argument("--image", default=os.path.join(DEFAULT_BUILD, "programs.img"))
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the .mox images")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool that writes the volume")
    parser.add_argument("--size-mb", type=int, default=IMAGE_MB)
    parser.add_argument(
        "--source-date-epoch",
        type=parse_epoch,
        default=os.environ.get("SOURCE_DATE_EPOCH", str(SOURCE_DATE_EPOCH)),
        metavar="N",
        help="the moment of the records (default: $SOURCE_DATE_EPOCH or 0)",
    )
    parser.add_argument(
        "--file",
        action="append",
        default=[],
        metavar="PATH=HOST",
        help="write a file of the host into the volume (mode 0644)",
    )
    parser.add_argument(
        "--program",
        action="append",
        default=[],
        metavar="PATH=HOST",
        help="write a file of the host into the volume with the mode of a program (0755)",
    )
    parser.add_argument(
        "--text",
        action="append",
        default=[],
        metavar="PATH=TEXT",
        help="write TEXT and a newline into the volume",
    )
    parser.add_argument(
        "--dir",
        action="append",
        default=[],
        metavar="PATH",
        help="make a directory of the volume",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    extra: list = []
    for pair in args.file:
        extra += ["--file", pair]
    for pair in args.program:
        extra += ["--program", pair]
    for pair in args.text:
        extra += ["--text", pair]
    for path in args.dir:
        extra += ["--dir", path]

    return build_image(
        args.image,
        args.build,
        args.mkfs,
        args.size_mb,
        args.source_date_epoch,
        args.verbose,
        extra,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
