#!/usr/bin/env python3
"""Micro OS — `make ports-smoke`: the gate of a program nobody rewrote.

The lane M3 of 1.8 promises a number and not a feeling: a program of POSIX
builds with this system's SDK **without a line of its source being edited**.
This test is that number for the console set of sbase - `true`, `false`,
`echo`, `basename`, `dirname`, `pwd`, `printenv`, `sleep`, `cksum`, `tee` and
`cmp` - and for the three calls of a command that 1.8 added to the library
(`popen`, `pclose`, `system`, proved by `/bin/ports/command`).

The run has two halves and both are the gate:

1. **The sources are the release.** Every file of `tests/ports/sbase/` is
   compared byte for byte with the sha256 of the upstream commit in
   `tests/ports/upstream.txt`. A hunk that somebody edited to make a tool
   build is therefore a failing check and not a sentence in a pull request.
   The programs themselves are built by `tools/ports_build.sh` with the SDK
   of `make sdk` and not by the CMake of this tree, which is the other half of
   the promise: no port layer of the tree is involved.

2. **Every program runs.** The programs are written into a volume of the
   machine under `/bin/ports/`, the machine boots, and each one is typed at
   the shell: the answer is the answer of the release (a word, a status, a
   checksum), and a tool that needed a patch to compile would fail to be
   there at all.

    tools/ports_smoke.py [--build build-ports] [--x86 build-x86]
                         [--timeout SECONDS] [--verbose]

The volume is built from the image of `make x86-disk` with the files of the
gate added to it, so the boot, the shell and `/etc` are the machine of the
release and only `/bin/ports` is new.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)

# The one machine of the smoke tests: the command line of QEMU, the prompt and
# the way a line is typed are the ones of `x86_smoke.py`, because a second
# copy of a machine is a second machine (and a second list of flags to keep).
sys.path.insert(0, TOOLS)
from x86_smoke import PROMPT, machine_argv, run_machine  # noqa: E402

# The programs of the gate: the file of the release, the name in `/bin/ports`
# and the commands typed for it. The list is the table of
# `docs/ports-table.md`, and `tests/ports/CMakeLists.txt` names the same tools
# (the build is the list of the programs, this is the list of the checks).
PROGRAMS = [
    "true",
    "false",
    "echo",
    "basename",
    "dirname",
    "pwd",
    "printenv",
    "sleep",
    "cksum",
    "tee",
    "cmp",
    "whoami",
    "sha256sum",
]

# The programs of the gate that a transcript cannot drive: `kilo` reads its
# keys one at a time in raw mode and a line of the harness is a line of the
# shell. It is built, written into the volume and hashed like the others, and
# the half of its gate that types a line, saves and reads the file back is
# `make tty-smoke` (tools/tty_smoke.py, ADR 0051 §10).
SCREEN_PROGRAMS = ["kilo"]

# The programs of this directory that are ours: the calls of a command, and
# the terminal asked in the words a transcript can check.
OWN_PROGRAMS = ["command", "ttyprobe"]

# The commands of the run, in the order they are typed. A comment in front of
# a group says what the group proves; the checks below read the answer of each
# line out of the transcript, so a line added here needs a check there.
COMMANDS = [
    # The two programs with no argument and a status: the whole of a program
    # of this system is its image, its `main` and the 0 it returns.
    "exec /bin/ports/true",
    "exec /bin/ports/false",
    # The formatter and the words of a line, with and without the newline.
    "exec /bin/ports/echo one two",
    "exec /bin/ports/echo -n no-newline",
    # The two halves of a path, and the suffix of `basename`.
    "exec /bin/ports/basename /etc/setting",
    "exec /bin/ports/basename /a/b.txt .txt",
    "exec /bin/ports/dirname /a/b/c",
    # The directory of the task through `getcwd` and a `stat` of "PWD".
    "exec /bin/ports/pwd",
    # The environment of a program: the four words of the kernel and one name
    # of them.
    "exec /bin/ports/printenv PATH",
    "exec /bin/ports/printenv",
    # The clock of the task: a sleep of no seconds returns at once.
    "exec /bin/ports/sleep 0",
    # The checksum of the file of the image: a fixed number of the host's
    # `cksum` for the same bytes (the CRC-32 of POSIX).
    "exec /bin/ports/cksum /etc/setting",
    # A filter of two descriptors: the pipe of the shell into `tee`, which
    # writes the file of the volume and the console with one `writeall`.
    "cat /etc/setting | /bin/ports/tee /tee.txt",
    "exec /bin/cat /tee.txt",
    # The user of the task through `getpwuid` of the library, and a program
    # that carries a library of its own (the hash of the collection).
    "exec /bin/ports/whoami",
    "exec /bin/ports/sha256sum /etc/setting",
    # Two files that are one and two files that are not: the message and the
    # status of a comparator.
    "exec /bin/ports/cmp /etc/setting /etc/setting",
    "exec /bin/ports/cmp /etc/setting /etc/columns",
    # The calls of a command: `system`, `popen` both ways and `pclose`, with
    # the programs of this same directory as the commands.
    "exec /bin/ports/command",
    # The terminal of a program (ADR 0051): the four questions a full-screen
    # program asks before it draws, and the answer of the console of the
    # machine.
    "exec /bin/ports/ttyprobe",
    # The listing of the directory is the list of the build itself.
    "exec /bin/ls /bin/ports",
]


def upstream_checks() -> list:
    """The sources of the gate are the release, byte for byte."""
    checks = []
    manifest = os.path.join(ROOT, "tests", "ports", "upstream.txt")

    if not os.path.isfile(manifest):
        return [("the manifest of the upstream is there", False, manifest)]

    listed = {}
    with open(manifest, encoding="utf-8") as file:
        for line in file:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            digest, _, path = line.partition("  ")
            listed[path] = digest

    root = os.path.join(ROOT, "tests", "ports")
    # Every file of a release of the gate is in the manifest: the sources of
    # sbase and the source and the license of kilo. `README.md` is ours and
    # says where the file came from, and a dot file (the `.clang-format` that
    # keeps the formatter away from sbase) is tooling of the tree and not a
    # byte of the release.
    present = sorted(
        os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/")
        for release in ("sbase", "kilo")
        for directory, _, names in os.walk(os.path.join(root, release))
        for name in names
        if name != "README.md" and not name.startswith(".")
    )

    checks.append(
        (
            "every source of the gate is in the manifest",
            present == sorted(listed),
            f"files {len(present)}, manifest {len(listed)}",
        )
    )

    for path, digest in sorted(listed.items()):
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            checks.append((f"{path} is the release", False, "missing"))
            continue
        with open(full, "rb") as file:
            got = hashlib.sha256(file.read()).hexdigest()
        checks.append((f"{path} is the release", got == digest, f"{got} != {digest}"))

    return checks


def answer(blocks: list, command: str, occurrence: int = 0) -> str:
    """The block of the transcript that follows the echo of a command."""
    indexes = [index for index, line in enumerate(COMMANDS) if line == command]
    if occurrence >= len(indexes):
        return ""
    index = indexes[occurrence]
    return blocks[index] if index < len(blocks) else ""


def line_is(text: str, word: str) -> bool:
    """A line of the transcript that is the word and nothing else."""
    return re.search(r"(?m)^" + re.escape(word) + r"\r?$", text) is not None


def check(output: str) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    parts = output.split(PROMPT)
    blocks = parts[1 : len(COMMANDS) + 1]

    expect(
        "the shell answers every command of the gate",
        len(blocks) == len(COMMANDS),
        f"{len(blocks)} answer(s) for {len(COMMANDS)} command(s)",
    )
    # The boot itself is the machine of the release: the volume was mounted,
    # the shell came up, and the programs of /bin are the ones of the image.
    expect("the volume of the machine is the disk", "the root volume is" in output)

    # --- the programs of the release ---------------------------------------
    true = answer(blocks, "exec /bin/ports/true")
    expect("true ends with 0", "the program exited with 0" in true, true)
    expect("true writes nothing", not re.search(r"(?m)^true\r?$", true), true)

    false = answer(blocks, "exec /bin/ports/false")
    expect("false ends with 1", "the program exited with 1" in false, false)

    echo = answer(blocks, "exec /bin/ports/echo one two")
    expect("echo writes its words", line_is(echo, "one two"), echo)

    echo_n = answer(blocks, "exec /bin/ports/echo -n no-newline")
    expect("echo -n leaves the newline out", "no-newline" in echo_n, echo_n)

    base = answer(blocks, "exec /bin/ports/basename /etc/setting")
    expect("basename writes the last part of a path", line_is(base, "setting"), base)

    base_suffix = answer(blocks, "exec /bin/ports/basename /a/b.txt .txt")
    expect("basename takes the suffix away", line_is(base_suffix, "b"), base_suffix)

    dirn = answer(blocks, "exec /bin/ports/dirname /a/b/c")
    expect("dirname writes the directory of a path", line_is(dirn, "/a/b"), dirn)

    pwd = answer(blocks, "exec /bin/ports/pwd")
    expect("pwd writes the directory of the task", line_is(pwd, "/"), pwd)

    env = answer(blocks, "exec /bin/ports/printenv PATH")
    expect("printenv writes a name of the environment", line_is(env, "/bin"), env)

    all_env = answer(blocks, "exec /bin/ports/printenv")
    expect(
        "printenv with no name writes the whole environment",
        "PATH=/bin" in all_env and "USER=root" in all_env,
        all_env,
    )

    sleep = answer(blocks, "exec /bin/ports/sleep 0")
    expect("sleep ends with 0", "the program exited with 0" in sleep, sleep)

    # The CRC-32 of POSIX over the 34 bytes of /etc/setting, as the `cksum`
    # of the host answers it: 1273462717 34.
    cksum = answer(blocks, "exec /bin/ports/cksum /etc/setting")
    expect(
        "cksum writes the CRC and the size of the file",
        re.search(r"(?m)^1273462717 34 ", cksum) is not None,
        cksum,
    )

    # --- the filter of two descriptors -------------------------------------
    tee = answer(blocks, "cat /etc/setting | /bin/ports/tee /tee.txt")
    expect("tee copies the stream to the console", "hostname=x86_64" in tee, tee)
    back = answer(blocks, "exec /bin/cat /tee.txt")
    expect(
        "tee wrote the file of the volume",
        "hello" in back and "console=vga" in back,
        back,
    )

    # The one account of the machine: uid 0 is root, and the name comes from
    # /etc/passwd through the library and not from the program.
    who = answer(blocks, "exec /bin/ports/whoami")
    expect("whoami writes the name of the user", line_is(who, "root"), who)

    # The SHA-256 of the 34 bytes of /etc/setting, as the `sha256sum` of the
    # host answers it: the program is the release and the library is sha256.c
    # of the collection.
    digest = answer(blocks, "exec /bin/ports/sha256sum /etc/setting")
    expect(
        "sha256sum writes the digest of the file",
        "639104026e7fe7471c72ee3cc8e9cd1ccb3a47372bec95317b0730d4a3cddb1f  /etc/setting"
        in digest,
        digest,
    )

    # --- the comparator ----------------------------------------------------
    same = answer(blocks, "exec /bin/ports/cmp /etc/setting /etc/setting")
    expect("cmp of a file with itself says nothing and ends with 0",
           "differ" not in same and "the program exited with 0" in same, same)
    differ = answer(blocks, "exec /bin/ports/cmp /etc/setting /etc/columns")
    expect(
        "cmp writes the byte and the line of the difference",
        "differ: byte 1, line 1" in differ,
        differ,
    )
    expect("cmp ends with 1 on a difference", "the program exited with 1" in differ, differ)

    # --- the three calls of a command --------------------------------------
    cmd = answer(blocks, "exec /bin/ports/command")
    expect("system runs a program of the image", line_is(cmd, "system true: 0"), cmd)
    expect("system answers the status of the command",
           line_is(cmd, "system false: 1"), cmd)
    expect("system splits the words of the line",
           line_is(cmd, "system echo: 0") and "first second word" in cmd, cmd)
    expect("system answers 127 for a program that is not there",
           line_is(cmd, "system missing: 127"), cmd)
    expect("system of an empty line is 0 and of NULL is 1",
           line_is(cmd, "system empty: 0") and line_is(cmd, "system null: 1"), cmd)
    expect("popen reads what the program wrote",
           line_is(cmd, "popen read: [one two]"), cmd)
    expect("pclose answers the status of the command it read",
           line_is(cmd, "pclose read: 0"), cmd)
    expect("popen of a program that is not there gives an empty stream",
           line_is(cmd, "popen missing: eof 1") and line_is(cmd, "pclose missing: 127"), cmd)
    expect("popen writes into the program of the pipe",
           line_is(cmd, "pclose write: 0"), cmd)
    expect("the child read the bytes the parent wrote",
           line_is(cmd, "read back: written through a pipe"), cmd)
    expect("the gate program ends with 0", "the program exited with 0" in cmd, cmd)

    # --- the terminal of a program (ADR 0051) ------------------------------
    tty = answer(blocks, "exec /bin/ports/ttyprobe")
    expect("the console is the terminal of the descriptors 0 and 1",
           line_is(tty, "tty: isatty(0)=1 isatty(1)=1 isatty(9)=0"), tty)
    expect("a terminal starts canonical with echo and the signal of Ctrl+C",
           line_is(tty, "tty: mode canonical=1 echo=1 isig=1 vmin=1 vtime=0"), tty)
    expect("TIOCGWINSZ answers the size of the screen",
           re.search(r"(?m)^tty: size [0-9]+x[0-9]+\r?$", tty) is not None, tty)
    expect("cfmakeraw clears the discipline of the line",
           line_is(tty, "tty: raw canonical=0 echo=0 isig=0 vmin=1 vtime=0"), tty)
    expect("the mode goes back to the one the program found",
           line_is(tty, "tty: back canonical=1 echo=1 isig=1 vmin=1 vtime=0"), tty)
    expect("/dev/tty is the same terminal under the name of POSIX",
           line_is(tty, "tty: /dev/tty isatty=1"), tty)
    expect("a file is not a terminal", line_is(tty, "tty: a file isatty=0"), tty)
    expect("the gate of the terminal ends with 0", "the program exited with 0" in tty, tty)

    # --- the directory of the build ----------------------------------------
    listing = answer(blocks, "exec /bin/ls /bin/ports")
    missing = [
        name for name in PROGRAMS + SCREEN_PROGRAMS + OWN_PROGRAMS if name not in listing
    ]
    expect("every program of the gate is a file of /bin/ports", not missing, f"missing {missing}")

    return checks


def build_image(image: str, x86: str, programs: str, verbose: bool) -> int:
    """The volume of the run: the tree of `make x86-disk` and /bin/ports."""
    mkfs = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
    if not os.path.isfile(mkfs):
        print(f"missing {mkfs}: run make build", file=sys.stderr)
        return 2

    command = [
        sys.executable,
        os.path.join(TOOLS, "x86_image.py"),
        "--image",
        image,
        "--build",
        x86,
        "--mkfs",
        mkfs,
        "--dir",
        "/bin/ports",
    ]
    for name in PROGRAMS + SCREEN_PROGRAMS + OWN_PROGRAMS:
        path = os.path.join(programs, f"{name}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run sh tools/ports_build.sh", file=sys.stderr)
            return 2
        command += ["--program", f"/bin/ports/{name}={path}"]

    # The license of the editor travels in the volume, as the license of every
    # program of the image does (ADR 0025 §4).
    command += ["--file", f"/etc/licenses/kilo={os.path.join(ROOT, 'tests', 'ports', 'kilo', 'LICENSE')}"]

    if verbose:
        print("image:", " ".join(command))

    return subprocess.call(command)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the gate of a program nobody rewrote")
    parser.add_argument("--build", default=os.path.join(ROOT, "build-ports"))
    parser.add_argument("--x86", default=os.path.join(ROOT, "build-x86"))
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument(
        "--disk",
        default="ide",
        choices=("ide", "virtio"),
        help="the bus of the disk of the run (default: ide)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    loader = os.path.join(args.x86, "loader32.elf")
    kernel = os.path.join(args.x86, "microos64.bin")
    programs = os.path.join(args.build, "programs")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    checks = upstream_checks()

    # A hunk edited into a source of the gate is the end of the gate: the
    # machine is not booted to run a program that is not the release, because
    # the numbers of the run would say nothing.
    broken = [item for item in checks if not item[1]]
    if broken:
        print("Micro OS — the gate of a program nobody rewrote (make ports-smoke)")
        print("=" * 72)
        for name, _, detail in broken:
            print(f"  FAIL  {name}")
            if detail:
                print(f"          {detail}")
        print("=" * 72)
        print("FAILED: the sources of tests/ports are not the release of the upstream")
        print("(tests/ports/upstream.txt carries the sha256 of the commit)")
        return 1

    image = os.path.join(args.build, "ports.img")
    run_image = os.path.join(args.build, "ports-run.img")

    for path in (image, run_image):
        if os.path.exists(path):
            os.remove(path)

    rc = build_image(image, args.x86, programs, args.verbose)
    if rc != 0:
        print("ports-smoke: the volume of the run was not written", file=sys.stderr)
        return rc

    # The commands of the run write the volume (/tee.txt): the test gets a
    # copy, so a run that failed in the middle leaves the image of the build
    # as the build wrote it.
    shutil.copyfile(image, run_image)

    started = time.monotonic()
    try:
        output = run_machine(loader, kernel, run_image, args.timeout, commands=COMMANDS,
                             disk=args.disk)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        if os.path.exists(run_image):
            os.remove(run_image)

    if args.verbose:
        print(output)

    checks += check(output)
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the gate of a program nobody rewrote (make ports-smoke)")
    print("=" * 72)
    for name, ok, detail in checks:
        if ok and not args.verbose:
            continue
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 72)
    print(f"ports: {len(PROGRAMS)} program(s) of sbase, {len(SCREEN_PROGRAMS)} of a screen, "
          f"{len(checks)} check(s), {time.monotonic() - started:.0f} s of the machine")

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        print("--- the console of the machine ---")
        print(output)
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
