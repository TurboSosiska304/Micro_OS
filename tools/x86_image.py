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
import shutil
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
    "streams",
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
    # The compression of the machine: the release's own command on top of the
    # library of the frame format (third_party/zstd).
    "zstd",
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
    # The type of a file, with libmagic (third_party/file). The database is a
    # file of /etc as well: the compiled subset and its text fallback.
    "file",
    # SQLite and the shell of the release (third_party/sqlite): the database
    # library of the public domain, its VFS on the calls of this system and
    # the program that runs the scripts of /etc.
    "sqlite3",
    "awk",
]

# The second name of the compression command: `zstdcat` is the same image and
# a different name in /bin, the way a Unix-like system links one program under
# the name of its mode. The port does not patch the release for it: `zstdcli.c`
# reads the name it was entered with and takes the mode of a known one
# (`zstdcat` unpacks to standard output).
SECOND_NAMES = {"zstdcat": "zstd"}

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
# the two runs of a reproducible build write the same bytes. The first line is
# a shebang because the rule of the magic database of `/bin/file` for a Lua
# script is the interpreter it names, and the interpreter skips that line.
HELLO_LUA = """#!/usr/bin/lua
-- /etc/hello.lua: the first script of the machine
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

# The dumper of the test of zstd: the bytes of a file as hexadecimal pairs on
# the console, so that a frame the guest wrote can travel out of the machine in
# a transcript of text (the console of the port drops the bytes of a binary
# stream and inserts a CR before every LF). The interpreter and its string
# library are the ones of `/bin/lua`.
HEX_LUA = """-- /etc/hex.lua: the bytes of a file as hexadecimal pairs, on the console.
local path = arg[1]
local file = assert(io.open(path, "rb"))
local data = file:read("a")
file:close()
local out = {}
for index = 1, #data do
    out[#out + 1] = string.format("%02x", data:byte(index))
end
print(table.concat(out))
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

# The data of the awk tests: three records of the kind a colon-separated
# file of a Unix-like system holds, and the first script of the interpreter.
# They are fixtures of the machine (the image tool writes the same bytes on
# every run), so the smoke test knows the answer of a sum and of a field.
COLUMNS = "root:0:0\nbin:1:1\nnik:1000:1000"

HELLO_AWK = """# /etc/hello.awk: the first awk script of the machine
BEGIN { FS = ":"; seen["root"] = 1; print "awk: hello from /etc/hello.awk" }
{ count++; sum += $3; if ($1 in seen) roots++ }
END { printf "awk: %d records, sum %d, root %d\\n", count, sum, roots }
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
ZSTD_LICENSE = os.path.join(ROOT, "third_party", "zstd", "LICENSE")
FILE_LICENSE = os.path.join(ROOT, "third_party", "file", "LICENSE")

# The database of libmagic, chosen and compiled by the build: the compact
# .mgc and the same text, the second level of the port when the compiled file
# is not there.
MAGIC_TEXT = "magic"
MAGIC_MGC = "magic.mgc"


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
AWK_LICENSE = os.path.join(ROOT, "third_party", "awk", "LICENSE")


def write_hello_lua(build_dir: str) -> str:
    """Write the script of the first Lua program, the same bytes every run."""
    path = os.path.join(build_dir, "hello.lua")

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(HELLO_LUA)

    return path

def write_fixture_zstd(build_dir: str) -> str:
    """The stream of `/etc/setting.zst`, compressed by the zstd of the host.

    The fixture is the other half of the test of the format: a frame written
    by the zstd of the machine that builds the image has to come out of
    `/bin/zstd` of Micro OS byte for byte, which is what "the same format"
    means. The level is the one a person would use (19) and the checksum is
    off, so two runs of the tool on one machine write the same bytes; the
    version of the host `zstd` is the one variable of the fixture, and the
    image tool prints the version it used.

    Python 3.14 has `compression.zstd` and is the fallback of a machine without
    the command: the frame of the two is the same format, which is the point.
    """
    path = os.path.join(build_dir, "setting.zst")
    source = os.path.join(build_dir, "setting.txt")
    text = (SETTING + "\n").encode("utf-8")

    # The text goes through a file and not a pipe: a frame of a stream whose
    # length is unknown names the window of its level (8 MiB at level 19),
    # and the machine that unpacks it is a program of a megabyte. The file is
    # the same bytes the volume carries, and the frame of it names a window of
    # the size of the text.
    with open(source, "wb") as file:
        file.write(text)

    zstd = shutil.which("zstd")
    if zstd is not None:
        result = subprocess.run(
            [zstd, "-19", "--no-check", "-q", "-f", "-o", path, source],
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError((result.stderr or b"").decode("utf-8", "replace").strip())
    else:
        from compression import zstd as pyzstd

        with open(path, "wb") as file:
            file.write(pyzstd.compress(text, level=19))

    # The same frame with its tail cut: the stream promises blocks that are
    # not there, and `zstd -t` of the guest has to refuse it. A test of a
    # decoder that only ever sees good frames proves half of the decoder.
    with open(path, "rb") as file:
        frame = file.read()
    broken = os.path.join(build_dir, "broken.zst")
    with open(broken, "wb") as file:
        file.write(frame[: len(frame) - 3])

    return path


def write_hello_lua(build_dir: str) -> str:
    """Write the script of the first Lua program, the same bytes every run."""
    path = os.path.join(build_dir, "hello.lua")

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(HELLO_LUA)

    return path


def write_hex_lua(build_dir: str) -> str:
    """Write the dumper of the test of zstd, the same bytes every run."""
    path = os.path.join(build_dir, "hex.lua")

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(HEX_LUA)

    return path


def write_hex_lua(build_dir: str) -> str:
    """Write the dumper of the test of zstd, the same bytes every run."""
    path = os.path.join(build_dir, "hex.lua")

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(HEX_LUA)

    return path


SQLITE_LICENSE = os.path.join(ROOT, "third_party", "sqlite", "LICENSE")

# The scripts of the database of the smoke tests. They are files of /etc and
# not a line of the shell: the shell of this machine has no quotes, and a
# statement of SQL has spaces. The first one builds the table in the first
# run of the program, the second one reads it in a second run and asks the
# library to check the file it found.
SQL_SETUP = (
    "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, price REAL,"
    " data BLOB);\n"
    "INSERT INTO items(name, price, data) VALUES('one', 1.5, x'0102');\n"
    "INSERT INTO items(name, price, data) VALUES('two', 2.25, x'03');\n"
    "BEGIN;\n"
    "INSERT INTO items(name, price, data) VALUES('roll', 9.0, x'ff');\n"
    "ROLLBACK;\n"
    "SELECT count(*) FROM items;\n"
    "SELECT id, name, price FROM items ORDER BY id;\n"
    "PRAGMA integrity_check;\n"
)
SQL_READ = (
    "SELECT count(*), sum(price) FROM items;\n"
    "SELECT typeof(price), typeof(data) FROM items WHERE id=1;\n"
    "PRAGMA integrity_check;\n"
)
SQL_NESTED = ".read /etc/sql-read.sql\n"
SQL_SCHEMA = ".tables\n.schema items\n.mode list\nSELECT name FROM items WHERE id=2;\n"
# The timer of the shell prints a real number with `%f` and a precision: the
# one line of the guest that uses the formatter of the library for a double.
SQL_TIMER = ".timer on\nSELECT count(*) FROM items;\n"


def write_awk_fixtures(build_dir: str) -> tuple:
    """Write the data and the script of the awk tests, the same bytes every run."""
    columns = os.path.join(build_dir, "columns")
    script = os.path.join(build_dir, "hello.awk")

    with open(columns, "w", encoding="utf-8", newline="\n") as file:
        file.write(COLUMNS + "\n")

    with open(script, "w", encoding="utf-8", newline="\n") as file:
        file.write(HELLO_AWK)

    return columns, script


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

    # The second names of the commands: one image, two names in /bin.
    for second, first in SECOND_NAMES.items():
        path = os.path.join(build_dir, f"{first}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        command += ["--program", f"/bin/{second}={path}"]

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
    if not os.path.isfile(ZSTD_LICENSE):
        print(f"missing {ZSTD_LICENSE}", file=sys.stderr)
        return 2
    if not os.path.isfile(FILE_LICENSE):
        print(f"missing {FILE_LICENSE}", file=sys.stderr)
        return 2

    # The database of /bin/file. Both levels are files of the image: the
    # compiled subset the build made and the text it was compiled from.
    for name in (MAGIC_TEXT, MAGIC_MGC):
        path = os.path.join(build_dir, name)
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    try:
        setting_zst = write_fixture_zstd(build_dir)
    except (OSError, RuntimeError) as error:
        print(f"the fixture of zstd: {error}", file=sys.stderr)
        return 1
    if not os.path.isfile(SQLITE_LICENSE):
        print(f"missing {SQLITE_LICENSE}", file=sys.stderr)
    if not os.path.isfile(AWK_LICENSE):
        print(f"missing {AWK_LICENSE}", file=sys.stderr)
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
    command += ["--file", f"/etc/hex.lua={write_hex_lua(build_dir)}"]
    command += ["--file", f"/etc/licenses/lua={LUA_LICENSE}"]

    # The compression of zstd: a stream the host wrote (the fixture the guest
    # unpacks), the same stream with its tail cut (the refusal of `-t`) and
    # the license of the library the command is linked against.
    command += ["--file", f"/etc/setting.zst={setting_zst}"]
    command += ["--file", f"/etc/broken.zst={os.path.join(build_dir, 'broken.zst')}"]
    command += ["--file", f"/etc/licenses/zstd={ZSTD_LICENSE}"]

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

    # libmagic and /bin/file: the notice and the two levels of the database.
    # The fixture of the Lua rule is `/etc/hello.lua` itself: the script of
    # the first program starts with the shebang the Magdir file of the
    # language names, and one file serves the two readers.
    command += ["--file", f"/etc/licenses/file={FILE_LICENSE}"]
    command += ["--file", f"/etc/magic={os.path.join(build_dir, MAGIC_TEXT)}"]
    command += ["--file", f"/etc/magic.mgc={os.path.join(build_dir, MAGIC_MGC)}"]
    # SQLite is in the public domain; the statement that says so travels with
    # the program of /bin/sqlite3, and the scripts the smoke tests run are
    # files of /etc.
    command += ["--file", f"/etc/licenses/sqlite={SQLITE_LICENSE}"]
    for name, text in (
        ("sql-setup.sql", SQL_SETUP),
        ("sql-read.sql", SQL_READ),
        ("sql-nested.sql", SQL_NESTED),
        ("sql-schema.sql", SQL_SCHEMA),
        ("sql-timer.sql", SQL_TIMER),
    ):
        path = os.path.join(build_dir, name)
        with open(path, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)
        command += ["--file", f"/etc/{name}={path}"]
    # The third ported program: the data, the script and the license of awk
    # are files of the volume, and the interpreter reads them through the VFS
    # like any other file of the machine.
    columns, awk_script = write_awk_fixtures(build_dir)
    command += ["--file", f"/etc/columns={columns}"]
    command += ["--file", f"/etc/hello.awk={awk_script}"]
    command += ["--file", f"/etc/licenses/awk={AWK_LICENSE}"]

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
