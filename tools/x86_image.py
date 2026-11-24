#!/usr/bin/env python3
"""Micro OS — the image of the disk of the x86-64 port.

The machine has no programs in its kernel any more: `/bin` and `/etc` are
objects of a volume on the disk, and the boot mounts that volume and runs
`/bin/hello` through `execve`. The volume is of MicroFS by default, written by
the host tool of the tree (`tools/mkfs_mfs.c`, which links `components/mfs`),
so the format has one implementation and not a second one in Python.

The machine of ADR 0027 boots a root of any filesystem of the tree:
`--fs ext2` writes the same `/bin` and `/etc` into an ext2 volume, with the
tools of the host (`mke2fs`), and the boot reads the type from the bytes.
That image carries a second partition as well, a FAT32 volume the machine
mounts at `/data` (`mkfs.fat`, dose tools).

The image is a whole disk: an MBR, one Linux partition at LBA 2048 and a
volume in it, with the `.mox` images of the build in `/bin`, an empty
`/etc/init.d` and the settings of the machine in `/etc/setting`. The MicroFS
volume is version 4 and carries the journal of ADR 0047; `--no-journal` makes
the version 3 volume of 1.8, which is the control half of the gate of a cut. The ext2
image is the same disk with a FAT32 partition after the root.

    tools/x86_image.py [--image PATH] [--build DIR] [--mkfs PATH]
                       [--fs mfs|ext2] [--size-mb N] [--data-mb N]
                       [--bus ide|virtio] [--volumes LIST] [--no-journal]
                       [--source-date-epoch N] [--verbose]

The bus is a property of the machine and not of the medium: the same bytes of
this image are the disk of the primary IDE channel of ADR 0008 and the disk of
the virtio-blk device of ADR 0037, and `--bus` says which of the two this run
of the tool is writing for. The image does not change; the tool prints the
`-drive` argument of the bus that was asked for, which is the line a person
puts in the command of QEMU.

The exit status is 0 when the image was written, 2 when the tool or one of
the images of a program is missing, and 1 when the tool failed.
"""

from __future__ import annotations

import argparse
import errno
import gzip
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile

# The programs of /bin, in the order of the list of the port. The name is
# the file in the build directory and the file of the volume.
PROGRAMS = [
    "hello",
    "crash",
    "fork",
    "badptr",
    "signals",
    "procs",
    # The gates of the first lane of 1.6: the wait for a descriptor, the
    # memory a program maps, and the measurement of a program of 4 MiB.
    "poll",
    "mmap",
    "big",
    # The lane of 1.8 of the library: the directory of the task (call 48),
    # the heap that grows with brk, and the memory limit a program charges it
    # to. The three are programs of the library and not of the port.
    "chdir",
    "heap",
    "limit",
    # The keyboard as events (ADR 0040): the smoke test of the USB reads the
    # code and the edge of a key out of /dev/input0 through this program.
    "keys",
    # The floating point state of a task (K9): three tasks compute in double
    # at once and the smoke test reads the number of each of them.
    "fpu2",
    "cat",
    "echo",
    "streams",
    "truncate",
    "ls",
    "wc",
    "head",
    "grep",
    "mkdir",
    "mv",
    "rm",
    "rmdir",
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
    # The first interface of the machine (ADR 0028): a program of the library
    # that draws in /dev/fb0 and reads /dev/input0. The editor and the file
    # manager of ADR 0030 are the programs above it.
    "ui",
    "edit",
    "fm",
    # The server of the desktop (1.7): the windows of the programs above as
    # clients of one program that owns the screen (docs/desktop.md).
    "wm",
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

# The accounts of the machine (ADR 0033): the five fields of a record of
# /etc/passwd and the salted SHA-256 of /etc/shadow. The password of root is
# `microos`, and the second account has none (`*`): only root becomes guest.
# The hash is the one `printf 'mox1%s' microos | sha256sum` answers, and the
# smoke test proves that the C side of the tree reads what Python wrote here.
ACCOUNTS_SALT = "mox1"
ACCOUNTS_ROOT_PASSWORD = "microos"
ACCOUNTS_PASSWD = "root:0:0:/:/bin/sh\nguest:7:7:/home/guest:/bin/sh"
# The groups of the machine (ADR 0044): `name:gid:member,member`, and the
# primary group of each account with no member list of its own - a task of
# this system has one gid and no supplementary group.
ACCOUNTS_GROUP = "root:0:\nguest:7:"


def accounts_shadow() -> str:
    """The text of /etc/shadow of the image, with the hash of the root."""
    digest = hashlib.sha256((ACCOUNTS_SALT + ACCOUNTS_ROOT_PASSWORD).encode("ascii")).hexdigest()

    return f"root:$sha256${ACCOUNTS_SALT}${digest}\nguest:*"


# The frame of `SETTING` and a newline: zstd 1.5.7, level 19, no checksum.
SETTING_ZST_FRAME = (
    "28b52ffd202211010068656c6c6f0a686f73746e616d653d7838365f36340a636f6e736f6c653d7667610a"
)

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

# The volumes this system may write (ADR 0041 §1): the list of /etc/volumes
# names the device a volume is mounted as, and one image of this tool boots on
# four buses - the IDE channel (ata0), the virtio card (virtio0), the SATA
# controller of a chipset (ahci0) and the NVMe namespace of a laptop (nvme0).
# The name of a volume is a fact of the machine at the moment of the mount and
# not of the bytes, so the list of one medium names every bus it may be
# attached to; a copy of the image without the line is read-only, which is the
# negative half of the gate. `--volumes` replaces the list, `--volumes none`
# writes no file at all.
VOLUMES = ("ata0p1", "virtio0p1", "ahci0p1", "nvme0p1")

VOLUMES_HEADER = "# the volumes of this system that a mount may write (ADR 0041)"

# The file of /etc the rule of the disks reads. The name is of the kernel
# (K_VOLUME_LIST_PATH) and the image tool writes the file where a mount finds
# it.
VOLUMES_PATH = "/etc/volumes"


def volumes_text(names) -> str:
    """The text of /etc/volumes: the header and one device name per line."""
    lines = [VOLUMES_HEADER]

    for name in names:
        lines.append(name)

    return "\n".join(lines)


IMAGE_MB = 16
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

    # The frame is a fixture, so it is bytes and not a call: `zstd` is a tool
    # of a development machine and the Python of the image of the release is
    # 3.10, which has no `compression.zstd`. The bytes below are the frame of
    # this exact text, made once by zstd 1.5.7 at level 19 without a checksum;
    # `zstd -t` of the machine reads it and gives the same text back.
    with open(path, "wb") as file:
        file.write(bytes.fromhex(SETTING_ZST_FRAME))

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
# The transaction a reset interrupts: the journal of the file is old pages of
# the table, and the line after it keeps the process alive long enough for the
# driver of the smoke test to take the machine down. The next boot rolls the
# journal back and reads the rows of the setup again.
SQL_TXN = (
    "BEGIN;\n"
    "UPDATE items SET price = 99.0 WHERE id = 1;\n"
    "INSERT INTO items(name, price, data) VALUES('extra', 42.0, x'ee');\n"
    "SELECT 'MIDTRANSACTION';\n"
    "WITH RECURSIVE c(x) AS (VALUES(1) UNION ALL SELECT x + 1 FROM c WHERE x < 20000000)"
    " SELECT sum(x) FROM c;\n"
)
SQL_AFTER = (
    "SELECT count(*) FROM items;\n"
    "SELECT price FROM items WHERE id = 1;\n"
    "SELECT count(*) FROM items WHERE name = 'extra';\n"
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


# --- the tree of the machine --------------------------------------------
#
# One list of objects is the tree of both images: /bin with the programs of
# the build, /etc with the settings and the licenses of the release. The
# writer of the format is the only thing that differs - MicroFS through the
# host tool of the tree (`tools/mkfs_mfs.c`, which links components/mfs), ext2
# through the tools of the host - so a file that is in one image is in the
# other, and a machine boots to the same shell whichever of the two it was
# given (ADR 0027 §3).
#
# An entry is (kind, path, payload, mode): the kind is "dir", "file" (a file
# of the host), "program" (a file of the host with the mode of a program) or
# "text" (text the writer puts in the file and a trailing newline).


def tree_dir(path: str, mode: int = 0o755) -> tuple:
    return ("dir", path, None, mode)


def tree_file(path: str, source: str, mode: int = 0o644) -> tuple:
    return ("file", path, source, mode)


def tree_program(path: str, source: str) -> tuple:
    return ("program", path, source, 0o755)


def tree_text(path: str, text: str, mode: int = 0o644) -> tuple:
    return ("text", path, text, mode)


def extra_entries(extra: list) -> list:
    """The `--dir/--file/--program/--text` arguments of the caller, as entries.

    The stress tools and the tests put a filler or a directory on the volume
    with the options of this tool; the tree of the two writers is one list,
    so those options are read here once and not twice.
    """
    entries = []
    index = 0

    while index + 1 < len(extra):
        option = extra[index]
        value = extra[index + 1]
        index += 2

        if option == "--dir":
            entries.append(tree_dir(value))
        elif option in ("--file", "--program", "--text"):
            path, _, payload = value.partition("=")
            if option == "--text":
                entries.append(tree_text(path, payload))
            elif option == "--program":
                entries.append(tree_program(path, payload))
            else:
                entries.append(tree_file(path, payload))

    return entries


# --- the writer of MicroFS ----------------------------------------------


def microfs_command(entries: list, image: str, size_mb: int, mkfs: str, epoch: int,
                    volumes: tuple = VOLUMES, journal: bool = True) -> list:
    """The call of the host tool that writes a MicroFS volume in the image.

    `mkfs` names the volumes it formatted: the list of /etc/volumes is the
    rule of ADR 0041, and the tool that makes the volume is the one place
    that knows the name a mount will ask for. `none` leaves the file out.
    """
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
        "--volumes",
        ",".join(volumes) if volumes else "none",
    ]

    # The volume of the release is version 4: the journal of ADR 0047. A
    # caller that wants the volume of 1.8 asks for none, which is the other
    # half of the gate of the format.
    if journal:
        command.append("--journal")

    for kind, path, payload, mode in entries:
        if kind == "dir":
            command += ["--dir", path]
        elif kind == "program":
            command += ["--program", f"{path}={payload}"]
        elif kind == "file":
            command += ["--file", f"{path}={payload}"]
        else:
            command += ["--text", f"{path}={payload}"]

        # The tool writes a directory of 0755 and a file of 0644 unless it is
        # told otherwise, and the mode comes after the object it is the mode
        # of (`/tmp` is 0777, /etc/shadow is 0600).
        default_mode = 0o755 if kind == "dir" else 0o644
        if mode != default_mode:
            command += ["--mode", f"{mode:04o}"]

    return command


# --- the writer of ext2 -------------------------------------------------
#
# The volume is written by the tools of the host, which are the ones that say
# what ext2 is: `mke2fs` makes the filesystem and copies the staged tree into
# it (`-d`), and the guest reads it with components/fs_ext2. A second writer
# of the format in this tree would be a second definition of it.
#
# The moment of every record is SOURCE_DATE_EPOCH and the UUID and the hash
# seed of the directories are fixed: the same inputs have to give the same
# bytes, which is the law of the image of the machine (make reproducible).

EXT2_START_LBA = 2048
EXT2_DEFAULT_MB = 64
EXT2_DATA_MB = 8

# The grain of the volume: the block of the format, the unit every record of
# ext2 is measured in. 1024 is the block of the image of the release, and the
# bytes of that image are the bytes of 1.7; 4096 is the grain of ADR 0042 - one
# operation of the block layer per block of the volume, and the metadata of the
# volume a quarter of the records for the same file - and a caller that measures
# it names it with `--block-size 4096` (`tools/x86_volume_smoke.py`,
# `tools/x86_bench.py`).
EXT2_BLOCK_BYTES = 1024
EXT2_BLOCK_CHOICES = (1024, 2048, 4096)
EXT2_UUID = "00000000-0000-4000-8000-000000000000"
EXT2_HASH_SEED = "01234567-89ab-cdef-0123-456789abcdef"

# The second partition of the image is the exchange volume of the machine: a
# FAT32 volume a person may write from any other computer, mounted at /data
# by the shell (`mount /dev/ata0p2 /data fat32`). Its label and its serial are
# fixed for the same reason as the UUID of the root.
DATA_LABEL = "MICROOS"
DATA_VOLUME_ID = "1a2b3c4d"

MBR_OFFSET = 446
MBR_ENTRY = 16
MBR_SIGNATURE = 510
MBR_TYPE_LINUX = 0x83
MBR_TYPE_FAT32_LBA = 0x0C


def write_mbr(file, entries: list) -> None:
    """The table of the disk image: (type, first LBA, length in LBAs) each.

    The four entries of an MBR, the CHS fields "use the LBA" (0xFE 0xFF 0xFF)
    the way every tool writes them when the geometry is not known, and the
    signature. The first entry carries the root and the second the data
    volume; a table of one entry is the image of a root alone.
    """
    sector = bytearray(512)

    for index, (kind, first, length) in enumerate(entries):
        base = MBR_OFFSET + index * MBR_ENTRY
        sector[base + 1] = 0xFE
        sector[base + 2] = 0xFF
        sector[base + 3] = 0xFF
        sector[base + 4] = kind
        sector[base + 5] = 0xFE
        sector[base + 6] = 0xFF
        sector[base + 7] = 0xFF
        sector[base + 8 : base + 12] = first.to_bytes(4, "little")
        sector[base + 12 : base + 16] = length.to_bytes(4, "little")

    sector[MBR_SIGNATURE] = 0x55
    sector[MBR_SIGNATURE + 1] = 0xAA

    file.seek(0)
    file.write(sector)


def copy_chunks(disk, file, offset: int, size: int) -> None:
    """Copy the source a megabyte at a time, leaving the zero blocks as holes.

    The bytes are read with `os.pread`, which does not move the position of
    the file object: a read through the buffer of Python after an `os.lseek`
    of the descriptor hands out the bytes of an earlier position, which is a
    copy that looks right and is not.
    """
    fd = file.fileno()
    position = 0

    while position < size:
        chunk = os.pread(fd, min(size - position, 1 << 20), position)
        if not chunk:
            break

        disk.seek(offset + position)

        if chunk.count(0) == len(chunk):
            disk.seek(len(chunk), 1)
        else:
            disk.write(chunk)

        position += len(chunk)


def copy_extents(disk, file, offset: int, size: int) -> bool:
    """Copy only the extents of the source that hold bytes.

    A volume of ext2 is mostly a hole: `mke2fs` writes the bitmaps and the
    tables of the groups and leaves the rest of the file unsized by the
    medium. Copying the extents and not the file is what makes a 5 GiB image
    cost the megabytes of the filesystem and not the 5 GiB of the volume, and
    the bytes of the image are the same either way. False means the host
    cannot tell data from holes (`SEEK_DATA` of this file), and the caller
    falls back to the megabyte loop.
    """
    fd = file.fileno()
    position = 0

    while position < size:
        try:
            start = os.lseek(fd, position, os.SEEK_DATA)
        except OSError as error:
            # ENXIO: the rest of the file is a hole, which is the end of the
            # data. Anything else is a filesystem without SEEK_DATA.
            return error.errno == errno.ENXIO

        end = os.lseek(fd, start, os.SEEK_HOLE)

        while start < end:
            chunk = os.pread(fd, min(end - start, 1 << 20), start)
            if not chunk:
                return True

            disk.seek(offset + start)

            if chunk.count(0) == len(chunk):
                disk.seek(len(chunk), 1)
            else:
                disk.write(chunk)

            start += len(chunk)

        position = end

    return True


def copy_into(disk, source: str, offset: int) -> None:
    """Copy a volume of the staging directory into the disk at `offset`."""
    with open(source, "rb") as file:
        size = os.fstat(file.fileno()).st_size

        if not copy_extents(disk, file, offset, size):
            copy_chunks(disk, file, offset, size)


def stage_tree(entries: list, root: str) -> None:
    """Write the tree of the volume into a directory of the host.

    `mke2fs -d` copies a directory and nothing else, so the objects of the
    list become files and directories here first, with the modes the volume
    is to carry. The owner is `root_owner=0:0` of mke2fs: the machine has one
    user and its files belong to it.
    """
    for kind, path, payload, mode in entries:
        target = os.path.join(root, path.lstrip("/"))

        if kind == "dir":
            os.makedirs(target, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(target), exist_ok=True)

            if kind == "text":
                with open(target, "w", encoding="utf-8", newline="\n") as file:
                    file.write(payload + "\n")
            else:
                shutil.copyfile(payload, target)

        os.chmod(target, mode)


def build_ext2_image(
    image: str,
    entries: list,
    size_mb: int = EXT2_DEFAULT_MB,
    data_mb: int = EXT2_DATA_MB,
    epoch: int = SOURCE_DATE_EPOCH,
    verbose: bool = False,
    block_size: int = EXT2_BLOCK_BYTES,
) -> int:
    """Write a disk whose root partition is an ext2 volume of the same tree.

    The layout is the one of the MicroFS image: the table in LBA 0, the root
    from LBA 2048, and - when `data_mb` is not zero - a FAT32 volume after it
    as the second partition, which the machine mounts at /data. The exit
    status is the one of the tool: 0 written, 2 a tool of the host is
    missing, 1 the format failed.
    """
    mke2fs = shutil.which("mke2fs")

    if mke2fs is None:
        print("missing mke2fs: install e2fsprogs", file=sys.stderr)
        return 2

    if data_mb > 0 and shutil.which("mkfs.fat") is None:
        print("missing mkfs.fat: install dosfstools", file=sys.stderr)
        return 2

    # The first megabyte is the table and the gap every tool leaves before a
    # partition; the root takes the rest but the data volume.
    root_mb = size_mb - data_mb - 1

    if root_mb < 4:
        print(f"--size-mb {size_mb} does not hold a root and {data_mb} MB of data", file=sys.stderr)
        return 1

    root_lba = EXT2_START_LBA
    root_lbas = root_mb * 2048
    data_lba = root_lba + root_lbas
    data_lbas = data_mb * 2048

    with tempfile.TemporaryDirectory(prefix="microos-ext2-") as work:
        staging = os.path.join(work, "root")
        os.makedirs(staging)
        stage_tree(entries, staging)

        volume = os.path.join(work, "root.ext2")

        with open(volume, "wb") as file:
            file.truncate(root_mb * 1024 * 1024)

        blocks = root_mb * 1024 * 1024 // block_size
        command = [
            mke2fs,
            "-q",
            "-F",
            "-t",
            "ext2",
            "-b",
            str(block_size),
            "-I",
            "256",
            "-m",
            "0",
            "-O",
            "^resize_inode",
            "-U",
            EXT2_UUID,
            "-E",
            f"hash_seed={EXT2_HASH_SEED},root_owner=0:0",
            "-d",
            staging,
            volume,
            str(blocks),
        ]

        if verbose:
            print(" ".join(command))

        # mke2fs stamps the superblock from the clock unless the environment
        # says which moment the build is: the same tree twice, the same bytes.
        environment = dict(os.environ, SOURCE_DATE_EPOCH=str(epoch))
        result = subprocess.run(command, capture_output=True, check=False, env=environment)

        if result.returncode != 0:
            message = (result.stderr or result.stdout or b"").decode("utf-8", "replace")
            print(f"mke2fs: {message.strip()}", file=sys.stderr)
            return 1

        data = None

        if data_mb > 0:
            data = os.path.join(work, "data.fat32")
            command = [
                "mkfs.fat",
                "-F",
                "32",
                "-n",
                DATA_LABEL,
                "-i",
                DATA_VOLUME_ID,
                # The label entry of the root carries a time of the day, and
                # the time of the run is not a fact of the tree: --invariant
                # is the constant the tool keeps for a reproducible medium.
                "--invariant",
                "-C",
                data,
                str(data_mb * 1024),
            ]

            if verbose:
                print(" ".join(command))

            result = subprocess.run(command, capture_output=True, check=False)

            if result.returncode != 0:
                message = (result.stderr or result.stdout or b"").decode("utf-8", "replace")
                print(f"mkfs.fat: {message.strip()}", file=sys.stderr)
                return 1

        os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)

        with open(image, "wb") as disk:
            disk.truncate(size_mb * 1024 * 1024)

            layout = [(MBR_TYPE_LINUX, root_lba, root_lbas)]
            if data is not None:
                layout.append((MBR_TYPE_FAT32_LBA, data_lba, data_lbas))

            write_mbr(disk, layout)
            copy_into(disk, volume, root_lba * 512)

            if data is not None:
                copy_into(disk, data, data_lba * 512)

    if verbose:
        print(f"ext2: {image}, {root_mb} MB of root, {data_mb} MB of data,"
              f" blocks of {block_size} bytes")

    return 0


# --- the image of the machine -------------------------------------------


def build_image(
    image: str,
    build_dir: str = DEFAULT_BUILD,
    mkfs: str = DEFAULT_MKFS,
    size_mb: int = IMAGE_MB,
    epoch: int = SOURCE_DATE_EPOCH,
    verbose: bool = False,
    extra: list | None = None,
    fs: str = "mfs",
    data_mb: int = EXT2_DATA_MB,
    block_size: int = EXT2_BLOCK_BYTES,
    volumes: tuple = VOLUMES,
    journal: bool = True,
) -> int:
    """Write the image with the programs of the build and /etc/setting.

    `extra` is a list of the arguments of the tool that a caller needs on top
    of the tree of the machine: `--file PATH=HOST`, `--text PATH=TEXT` and
    `--dir PATH`. The stress tools use it to put a filler on the volume and
    reach `ENOSPC` in a boot; the image of a person stays the default tree.

    `fs` is the filesystem of the root: "mfs" (the default of the machine,
    written by `tools/mkfs_mfs.c`) or "ext2" (the same tree in a volume of the
    tools of the host, ADR 0027 §3).
    """
    if not os.path.isfile(mkfs):
        print(f"missing {mkfs}: run make build", file=sys.stderr)
        return 2

    entries = [
        tree_dir("/bin"),
        tree_dir("/etc"),
        tree_dir("/etc/init.d"),
        tree_dir("/etc/licenses"),
        # The mount points of the virtual filesystems: the boot mounts /proc
        # and /dev over these empty directories, and a listing of the root
        # that reads the volume (the `ls` of sbase) then finds the two names
        # as well as a listing of the shell that asks the VFS.
        tree_dir("/proc"),
        tree_dir("/dev"),
    ]

    for name in PROGRAMS:
        path = os.path.join(build_dir, f"{name}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        entries.append(tree_program(f"/bin/{name}", path))

    # The second names of the commands: one image, two names in /bin.
    for second, first in SECOND_NAMES.items():
        path = os.path.join(build_dir, f"{first}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        entries.append(tree_program(f"/bin/{second}", path))

    for license_file in (ZLIB_LICENSE, LUA_LICENSE, SBASE_LICENSE, BC_LICENSE, ZSTD_LICENSE,
                         FILE_LICENSE, SQLITE_LICENSE, AWK_LICENSE):
        if not os.path.isfile(license_file):
            print(f"missing {license_file}", file=sys.stderr)
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

    # The fixture of zcat and the license that travels with its bytes. The
    # program is part of the image, so the image carries the notice of the
    # library it was linked against; a package that ships zcat copies the same
    # text beside the program.
    greeting = write_greeting(build_dir, epoch)
    entries.append(tree_file("/etc/greeting.gz", greeting))
    entries.append(tree_file("/etc/licenses/zlib", ZLIB_LICENSE))

    # The script of Lua and the license of its library. The script is a file
    # of the volume and not a line of a test: the program reads it through
    # the VFS and the smoke test reads the lines the program printed.
    script = write_hello_lua(build_dir)
    entries.append(tree_file("/etc/hello.lua", script))
    entries.append(tree_file("/etc/hex.lua", write_hex_lua(build_dir)))
    entries.append(tree_file("/etc/licenses/lua", LUA_LICENSE))

    # The compression of zstd: a stream the host wrote (the fixture the guest
    # unpacks), the same stream with its tail cut (the refusal of `-t`) and
    # the license of the library the command is linked against.
    entries.append(tree_file("/etc/setting.zst", setting_zst))
    entries.append(tree_file("/etc/broken.zst", os.path.join(build_dir, "broken.zst")))
    entries.append(tree_file("/etc/licenses/zstd", ZSTD_LICENSE))

    # The MIT notice of sbase travels with the ten commands of /bin, as the
    # notice of zlib travels with zcat.
    entries.append(tree_file("/etc/licenses/sbase", SBASE_LICENSE))

    # The two calculators: the scripts of /etc and the license of the library
    # that both images carry. bc 7.2.0 is one engine with two names, so one
    # notice of the release is beside the two programs.
    entries.append(tree_file("/etc/bc.bc", write_bc_script(build_dir)))
    entries.append(tree_file("/etc/dc.dc", write_dc_script(build_dir)))
    entries.append(tree_file("/etc/licenses/bc", BC_LICENSE))

    # libmagic and /bin/file: the notice and the two levels of the database.
    # The fixture of the Lua rule is `/etc/hello.lua` itself: the script of
    # the first program starts with the shebang the Magdir file of the
    # language names, and one file serves the two readers.
    entries.append(tree_file("/etc/licenses/file", FILE_LICENSE))
    entries.append(tree_file("/etc/magic", os.path.join(build_dir, MAGIC_TEXT)))
    entries.append(tree_file("/etc/magic.mgc", os.path.join(build_dir, MAGIC_MGC)))

    # SQLite is in the public domain; the statement that says so travels with
    # the program of /bin/sqlite3, and the scripts the smoke tests run are
    # files of /etc.
    entries.append(tree_file("/etc/licenses/sqlite", SQLITE_LICENSE))

    for name, text in (
        ("sql-setup.sql", SQL_SETUP),
        ("sql-read.sql", SQL_READ),
        ("sql-nested.sql", SQL_NESTED),
        ("sql-schema.sql", SQL_SCHEMA),
        ("sql-timer.sql", SQL_TIMER),
        ("sql-txn.sql", SQL_TXN),
        ("sql-after.sql", SQL_AFTER),
    ):
        path = os.path.join(build_dir, name)
        with open(path, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)
        entries.append(tree_file(f"/etc/{name}", path))

    # The third ported program: the data, the script and the license of awk
    # are files of the volume, and the interpreter reads them through the VFS
    # like any other file of the machine.
    columns, awk_script = write_awk_fixtures(build_dir)
    entries.append(tree_file("/etc/columns", columns))
    entries.append(tree_file("/etc/hello.awk", awk_script))
    entries.append(tree_file("/etc/licenses/awk", AWK_LICENSE))

    entries.append(tree_text("/etc/setting", SETTING))

    # The accounts of the machine: the two files of ADR 0033, and the home
    # directory of the second user, so that `su - guest` has a directory to
    # enter. The hash of the password of root is written by this tool; the
    # shell checks it with the SHA-256 of the tree. `/tmp` is the directory
    # every user writes: a file that a user creates is the debt of
    # docs/debts.md (the create path gives it to root), and the smoke test
    # reads the owner of one of them.
    entries.append(tree_text("/etc/passwd", ACCOUNTS_PASSWD))
    entries.append(tree_text("/etc/group", ACCOUNTS_GROUP))
    entries.append(tree_text("/etc/shadow", accounts_shadow(), 0o600))
    entries.append(tree_dir("/home"))
    entries.append(tree_dir("/home/guest"))
    entries.append(tree_dir("/tmp", 0o777))

    license_path = os.path.join(ROOT, PCRE2_LICENSE)
    if not os.path.isfile(license_path):
        print(f"missing {license_path}", file=sys.stderr)
        return 2
    entries.append(tree_file("/etc/licenses/pcre2", license_path))

    # The roots of the machine (ADR 0024 §3): one file of PEM certificates in
    # /etc, so that a fresh image can speak https - `pkg -Sy` of a repository
    # over TLS is the documented install path, and a machine with no store
    # trusts nobody and says so. `tools/ca/README.md` names the source and the
    # way to update the bundle; a person replaces the file with `wget`.
    ca_bundle = os.path.join(ROOT, "tools", "ca", "mozilla.pem")
    if not os.path.isfile(ca_bundle):
        print(f"missing {ca_bundle}", file=sys.stderr)
        return 2
    entries.append(tree_dir("/etc/ssl"))
    entries.append(tree_file("/etc/ssl/certs.pem", ca_bundle))

    # The key of the repository of the project (docs/wiki/pkg.md): the file
    # the documentation names for `<repo>.pub`. Without it the key of the
    # image answers, and the key of an image built from this tree is the one
    # generated in its build directory - so `pkg -S hello` of the public
    # repository failed on every image a person builds. `tools/pkg/README.md`
    # names the source.
    repo_key = os.path.join(ROOT, "tools", "pkg", "microos-progs.pub")
    if not os.path.isfile(repo_key):
        print(f"missing {repo_key}", file=sys.stderr)
        return 2
    entries.append(tree_dir("/etc/pkg"))
    entries.append(tree_dir("/etc/pkg/keys"))
    entries.append(tree_file("/etc/pkg/keys/microos-progs.pub", repo_key))

    entries += extra_entries(extra or [])

    if fs == "ext2":
        # The data volume of the ext2 image has a place in the tree: an empty
        # /data, where the shell mounts the second partition of the disk.
        if data_mb > 0:
            entries.append(tree_dir("/data"))

        # The writer of the ext2 volume is mke2fs and not the host tool of
        # MicroFS, so the list of the volumes is an object of the tree here;
        # the tool is the one authority on what it says (volumes_text).
        if volumes:
            entries.append(tree_text(VOLUMES_PATH, volumes_text(volumes)))

        return build_ext2_image(image, entries, size_mb, data_mb, epoch, verbose, block_size)

    command = microfs_command(entries, image, size_mb, mkfs, epoch, volumes, journal)

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
    parser.add_argument(
        "--fs",
        choices=("mfs", "ext2"),
        default="mfs",
        help="the filesystem of the root partition (default: mfs)",
    )
    parser.add_argument(
        "--size-mb",
        type=int,
        default=None,
        help=f"the size of the disk (default: {IMAGE_MB} MB, {EXT2_DEFAULT_MB} MB for ext2)",
    )
    parser.add_argument(
        "--data-mb",
        type=int,
        default=EXT2_DATA_MB,
        metavar="N",
        help=f"the FAT32 /data partition of the ext2 image (default: {EXT2_DATA_MB}, 0: none)",
    )
    parser.add_argument(
        "--block-size",
        type=int,
        choices=EXT2_BLOCK_CHOICES,
        default=EXT2_BLOCK_BYTES,
        metavar="N",
        help="the block of the ext2 volume in bytes (default: 1024; MicroFS is 1024)",
    )
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
    parser.add_argument(
        "--bus",
        choices=("ide", "virtio"),
        default="ide",
        help="the bus the image is booted on (default: ide); the bytes are the same",
    )
    parser.add_argument(
        "--volumes",
        default=None,
        metavar="LIST",
        help="the device names of /etc/volumes, comma separated "
        f"(default: {','.join(VOLUMES)}; `none` writes no list and the volume "
        "is read-only)",
    )
    parser.add_argument(
        "--no-journal",
        action="store_true",
        help="make a version 3 volume (no journal, the format of 1.8)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.size_mb is None:
        args.size_mb = EXT2_DEFAULT_MB if args.fs == "ext2" else IMAGE_MB

    if args.no_journal and args.fs == "ext2":
        print("--no-journal is the format of MicroFS: ext2 has no journal yet", file=sys.stderr)
        return 2

    if args.block_size != EXT2_BLOCK_BYTES and args.fs != "ext2":
        print("--block-size is the grain of the ext2 volume: MicroFS counts 1024", file=sys.stderr)
        return 2

    extra: list = []
    for pair in args.file:
        extra += ["--file", pair]
    for pair in args.program:
        extra += ["--program", pair]
    for pair in args.text:
        extra += ["--text", pair]
    for path in args.dir:
        extra += ["--dir", path]

    if args.volumes is None:
        volumes = VOLUMES
    elif args.volumes == "none" or args.volumes == "":
        volumes = ()
    else:
        volumes = tuple(name.strip() for name in args.volumes.split(",") if name.strip())

    rc = build_image(
        args.image,
        args.build,
        args.mkfs,
        args.size_mb,
        args.source_date_epoch,
        args.verbose,
        extra,
        args.fs,
        args.data_mb,
        args.block_size,
        volumes,
        not args.no_journal,
    )

    if rc == 0:
        # The image is a file of the host and the bus is the machine's: the
        # screen of this tool is the one place that says which line boots the
        # bytes it just wrote.
        if args.bus == "virtio":
            line = "if=virtio"
        else:
            line = "if=ide,index=0"
        print(f"bus: {args.image} boots as -drive file={args.image},format=raw,{line},"
              f"cache=writeback")

    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
