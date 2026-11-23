#!/usr/bin/env python3
"""Micro OS — the smoke test of the x86-64 port.

The kernel of the host build is a process, and its smoke test runs it. This
one boots the kernel on a real (emulated) processor: QEMU loads the loader
of the port, the loader turns the long mode on, copies the kernel to 16 MiB
and maps it at `0xFFFFFFFF80000000`, and the kernel prints its banner on the
serial port. There is no operating system under it and no libc.

Then the shell has to answer: the commands are typed into the machine once
the console is up, and their answers are read back from the same transcript.
The prompt paces the typing on its own — the next command goes out when the
shell prints the prompt of the previous one — so a line is never split by
the small FIFO of the serial port of the emulated chip.

Between the boot and the shell the port of this machine brings up the
filesystem of the disk and runs a program out of it: the image of the machine
carries a MicroFS volume with /bin and /etc already written (tools/mkfs_mfs.c,
`make x86-disk`), the boot mounts it at / and asks for /bin/hello with execve.
Nothing is written to the volume at the boot; the program is a file the tool
left there. The program is not a creature of the port either: it prints
through the system call `write`, asks the kernel its pid and the time, sleeps
and returns a status, and every line of it below is a line the kernel
produced for it (ADR 0006). The transcript is the proof: a kernel that only
printed a banner could not have a number of a task in the middle of it.

    tools/x86_smoke.py [--image PATH] [--timeout SECONDS] [--verbose]

The run also reads the ELF of the kernel and checks that the image is linked
in the higher half (ADR 0007): a linker script that fell back to the physical
address boots the same and would pass every line of the transcript, so the
symbols `_start64` and `__kernel_end` are checked against the base.

The exit status is 0 when the kernel came up, the program ran out of a file
and the shell answered, and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

# The commands that are typed into the shell. The answers are read by their
# place in the list, the way the smoke test of the simulator does it. The last
# one looks at the tree the boot left behind: a program that ran out of a file
# is a program that is still in /bin afterwards.
COMMANDS = [
    "help",
    "kill 9999",
    "uname",
    "uptime",
    "ulimit",
    "ls /bin",
    # The one file of /etc is on the disk as well, and reading it is the
    # proof that the tree of the image is the tree of the machine.
    "cat /etc/setting",
    # The systems of /etc (ADR 0021): the defaults the boot made on the
    # volume, the key read back, and the one system this machine applies.
    # The panel and the font are the board's, so their values are written and
    # kept with a sentence that says so; the console is the attribute of the
    # VGA and the boot says which one it put on the screen.
    "config list",
    "config",
    "config sync",
    "config get display.rotation",
    "config set console.theme light",
    "config get console.theme",
    "config set display.invert 0",
    "config get display.invert",
    "cat /etc/display",
    "chmod 0644 /bin/hello",
    "exec /bin/hello",
    "chmod 0755 /bin/hello",
    "chown 0 /bin/hello",
    "exec /bin/hello",
    "exec /bin/hello one two",
    # The programs of the library (ADR 0016): a file through open/read/
    # write and the formatter, a line of arguments, and a directory through
    # readdir. The file they read is /etc/setting of the image.
    # The shell lists the names of a directory, and the tree is a command of
    # its own: `ls` has no branches, `tree` has them and counts what it drew.
    "ls /",
    "tree /",
    "tree -d /",
    # The virtual filesystems of the tree: the information about the system
    # and the devices, mounted by the boot like the board and the simulator
    # mount them. A machine of this system has the same root.
    "ls /proc",
    "cat /proc/version",
    "ls /dev",
    "exec /bin/cat /etc/setting",
    "exec /bin/cat /no/such/file",
    "exec /bin/echo one two",
    "exec /bin/ls /bin",
    # The utilities of a line (ADR 0016): a count, the first lines and a
    # search, each over the file the image carries. The bytes of the file are
    # "hello\nhostname=x86_64\nconsole=vga\n", so wc answers three lines and
    # 34 bytes - a number that comes out of the file and not out of the
    # program's own text.
    "exec /bin/wc /etc/setting",
    "exec /bin/wc -l /etc/setting",
    "exec /bin/wc -c /etc/setting",
    "exec /bin/head -n 1 /etc/setting",
    "exec /bin/grep hello /etc/setting",
    "exec /bin/grep -i HELLO /etc/setting",
    # The regular expression engine of the tree (third_party/pcre2): a pattern
    # with a wildcard and a folded case, then one with an anchor and a group.
    # The quotes of the examples of the wiki are not needed: the shell has no
    # quotes, and a word is the pattern as it is typed.
    "exec /bin/pcre2grep -i h.LLO /etc/setting",
    "exec /bin/pcre2grep -o -g 1 ^hostname=(x86_64)$ /etc/setting",
    "exec /bin/ls /etc/licenses",
    # The same commands with no file: they read descriptor 0, which the shell
    # does not pass to a program yet, so the call answers EBADF (9). The line
    # is the proof that the input is descriptor 0 and not a usage sentence.
    "exec /bin/cat",
    "exec /bin/wc -l -",
    # The tree through the calls: a directory made, a directory refused by
    # rm without -r, the tree taken down with the flag, and a listing of a
    # directory that is no longer there.
    "mkdir /tmp",
    "exec /bin/mkdir /tmp/d",
    "exec /bin/ls /tmp",
    "exec /bin/rm /tmp/d",
    "exec /bin/rm -r /tmp",
    "exec /bin/ls /tmp",
    "exec /bin/pwd",
    # A program that starts another program with an environment of its own:
    # the third argument of execve copied from the memory of a program, not
    # the four words of the kernel that every call without one gets.
    "exec /bin/spawn",
    # The pipes and the redirects of K7 (ADR 0017). The shell is one
    # interpreter on every machine, so these lines are the same that the
    # host tests run; here the last stage is a program of ring 3 as well,
    # and it reads the descriptor the stage built for it.
    "mkdir /tmp",
    # The first port of a program of somebody else (zlib): the .gz is a file
    # of the image, zcat inflates it with the library of the guest, and the
    # bytes reach a file of MicroFS and the console. `wc -c` is the count of
    # the fixture and `cat` the text, so a copy that lost a byte is visible.
    "exec /bin/zcat -o /tmp/greeting.txt /etc/greeting.gz",
    "exec /bin/wc -c /tmp/greeting.txt",
    "exec /bin/cat /tmp/greeting.txt",
    "exec /bin/zcat -t /etc/greeting.gz",
    # The verbose line is the `%llu` of the formatter of the program and the
    # count of the stream, on the error descriptor and not in the data.
    "exec /bin/zcat -v -t /etc/greeting.gz",
    "exec /bin/zcat /etc/no-such.gz",
    # The same stream through a pipe: zcat reads descriptor 0 and the port
    # layer waits out the EAGAIN of the producer that is still writing.
    "cat /etc/greeting.gz | /bin/zcat",
    "exec /bin/ls /etc/licenses",
    # The second port of a program of somebody else (Lua): the interpreter is
    # a file of /bin, its first script and its license are files of /etc, and
    # the number of a program with a point in it is the arithmetic of the
    # library of the guest. The image is 164 KiB and K_EXEC_MAX_IMAGE (8 MiB)
    # lets sys_execve read it in pieces of KSYS_MAX_IO; the run of the
    # library is the host test `tests/cases/test_lua.c`.
    "exec /bin/lua -e print(2^10)",
    "exec /bin/lua -e print(math.pi)",
    'exec /bin/lua -e print(string.format("%.3f",1/3))',
    "exec /bin/lua -e print(1/0,0/0)",
    "exec /bin/lua -e print(os.time()>=0)",
    "exec /bin/lua /etc/hello.lua",
    "exec /bin/lua /etc/no-such.lua",
    "exec /bin/cat /etc/hello.lua",
    # The third port of a program of somebody else (bc 7.2.0): the calculator
    # of arbitrary precision and its RPN sibling dc, one engine and two names
    # of /bin. The image is 116 KiB and its arena is the bss (768 KiB); the
    # library the two carry is the one of the release, and `-l` is its math
    # library, generated into the image from `gen/lib.bc`.
    "exec /bin/bc -e 2^100",
    "exec /bin/bc -l -e scale=10 -e 4*a(1)",
    "exec /bin/bc -e ibase=16 -e FF",
    "exec /bin/bc -e obase=2 -e 10",
    "exec /bin/bc -l -f /etc/bc.bc < /dev/null",
    # The same script on descriptor 0: the shell opens the file and the
    # calculator reads its input the way the release does, line by line.
    "exec /bin/bc -l < /etc/bc.bc",
    "exec /bin/dc /etc/dc.dc",
    "exec /bin/dc -e 2 -e 3+p",
    "exec /bin/cat /etc/bc.bc",
    "echo hi > /tmp/f",
    "cat /tmp/f",
    "cat < /tmp/f",
    "echo one | cat",
    "echo one | /bin/cat",
    "cat /etc/setting | wc -l",
    "ls /bin | wc -l",
    # A line that cannot run is a sentence, and the shell answers the next
    # command: a `cd` has no meaning in a stage, and a file that is not
    # there is the failure of that stage only.
    "cd / | cat",
    "cat /no/such/file | /bin/cat",
    "fsck",
    # The program of K4: it forks, the child writes the shared page and ends
    # with 7, the parent waits and still reads its own 42 (copy on write).
    "exec /bin/fork",
    # The same again, after the page tables and the frames of the first run
    # have been released: a leak of a space would show here as a refusal.
    "exec /bin/fork",
    # The signals of K7: a handler of the program, a child ended by SIGTERM,
    # a wait cut short by SIGINT and the errors of kill.
    "exec /bin/signals",
    # The edge of the system call: the program hands write, stat and read
    # addresses that are not its memory, and every call has to answer EFAULT
    # (14) while the machine stays alive.
    "exec /bin/badptr",
    # The pages of a program (ADR 0007, NX and W^X): the text is executable
    # and not writable, the data and the stack are writable and not
    # executable. A child that fetches its data, a child that writes its text
    # and a child whose text is the buffer of read() are all taken down by the
    # fault handler with the error code of the access - 0x15 with the
    # instruction-fetch bit, 0x7 for a write - and the parent is still alive
    # to say so.
    "exec /bin/nx",
    # The program that faults: it writes to address zero in ring 3, and the
    # shell has to answer the command after it for the machine to be alive.
    "exec /bin/crash",
    "uname",
    # The files of the machine: the mode is octal (the x86 kernel's printf
    # learned %o), the root of the tree cannot be unmounted, and `free` is
    # the memory and not the clock.
    "stat /etc/setting",
    "umount /",
    "umount /proc",
    "df /nope",
    "path \"\"",
    "ls /",
    "free",
    # `.` and `..` are the directory and its parent, and repeated or
    # trailing slashes are the same path: the shell normalizes before a call.
    "cd /etc",
    "pwd",
    "cd .",
    "pwd",
    "cd //etc//",
    "pwd",
    "cd ..",
    "pwd",
    "ls ..",
    "cd /",
    "memtest",
    # The tools of sbase (third_party/sbase) are the commands of the machine
    # now: `ls`, `cat`, `grep`, `wc` and `head` are the files of /bin and not
    # the small programs of ADR 0016, and `tail`, `sort`, `find`, `xargs` and
    # `sed` are new. Both the bare name (PATH-first of the shell) and the
    # explicit file are checked, because the two are one contract.
    "ls -la /etc",
    "find /etc -name setting",
    "sort /etc/setting",
    "grep -r hostname /etc",
    "grep -r -i VGA /etc",
    "wc -l /etc/setting",
    "tail -n 1 /etc/setting",
    "head -n 1 /etc/setting",
    "sed -n s/x86_64/X/p /etc/setting",
    "cat /etc/setting",
    # `xargs` passes its words to one `execve`, and the ABI of the call allows
    # at most eight words in the vector (docs/syscall-abi.md); -n 4 keeps the
    # batch inside it. The limit is a debt of the ABI, not a rule of sbase.
    "ls /etc | xargs -n 4",
    "ls --color /etc",
    "ls --color=never /etc",
    "exec /bin/ls -la /etc",
    "exec /bin/find /etc -name setting",
]

# A directory wider than the table of 64 entries the shell used to keep: the
# files are written one command at a time, and the listing and the walk have
# to name and count all of them. `rm -r` puts the volume back for the checks
# that follow (the run works on a copy of the image, but a clean tree is
# easier to read in a failure).
MANY_FILES = 70
COMMANDS += ["mkdir /many"]
COMMANDS += [f"write /many/f{index:02d} x" for index in range(MANY_FILES)]
COMMANDS += ["ls /many", "tree /many", "rm -r /many"]

# The prompt of the shell. It is written in one call, so the log of another
# task never splits it, and the answers can be separated by it.
PROMPT = "microos> "

# What the kernel has to say before the shell exists. The banner is the proof
# that the whole chain works: the loader, the switch to long mode, the
# console of COM1, the contract of the port and the boot of the kernel.
EXPECTED = [
    # The loader checked the trailer of the kernel before it copied a byte of
    # it (ADR 0020); the line is the proof that the check ran and passed.
    "Micro OS: image signature ok (sha-256, ed25519)",
    "board: qemu_x86_64",
    "port: x86_64, contract 1.9",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
    # The port reads the tables back at the boot: the text of the kernel is
    # read-only, every data page is NX, and no page is both (ADR 0007).
    "x86-64: the map is W^X",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")

# The number of processors of the machine of the smoke test (ADR 0012). It is
# a constant of the test and not a setting: the point of the line is that the
# firmware describes two and the kernel knows how many it has started.
MACHINE_SMP = "2"
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")
RUN_IMAGE = os.path.join(BUILD, "programs-smoke.img")

# The base of the higher half (ADR 0007): the kernel is linked there and the
# loader copies the bytes to 16 MiB. The two are the same boot when the
# linker script falls back to the physical address - the kernel would run in
# the low identity map and every line of this transcript would still appear -
# so the link address is read out of the ELF and not out of the console.
KERNEL_BASE = 0xFFFFFFFF80000000
KERNEL_ELF = os.path.join(BUILD, "microos64")


def linker_checks(elf_path: str) -> list:
    """The checks that the kernel is linked where the port says it is.

    A symbol of the image is the only thing that tells the link address from
    the address the loader copied the bytes to; nm and readelf are the tools
    of the same binutils as the ld and objcopy of the build.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    try:
        header = subprocess.run(
            ["readelf", "-h", elf_path], capture_output=True, text=True, check=True
        ).stdout
        symbols = subprocess.run(
            ["nm", "-n", elf_path], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        expect("the ELF of the kernel is readable", False, str(error))
        return checks

    def symbol(name: str):
        for line in symbols.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == name:
                return int(parts[0], 16)
        return None

    entry = re.search(r"Entry point address:\s*(0x[0-9a-fA-F]+)", header)
    entry = int(entry.group(1), 16) if entry else None
    start = symbol("_start64")
    end = symbol("__kernel_end")

    # The entry point of the ELF and the first symbol of the image are the
    # same address by the linker script; both have to be the high base.
    expect(
        "the kernel is linked in the higher half",
        entry == KERNEL_BASE and start == KERNEL_BASE,
        f"entry {entry!r}, _start64 {start!r}, expected {KERNEL_BASE:#x}",
    )
    expect(
        "the image stays in the top 2 GiB",
        end is not None and KERNEL_BASE < end < KERNEL_BASE + 0x80000000,
        f"__kernel_end {end!r}",
    )

    return checks


def run_refused_boot(loader: str, kernel: str, timeout: float = 10.0) -> str:
    """Boot an image the loader should refuse, and return what it said.

    There is no shell and no disk: a good image would reach the kernel and
    print its banner, a bad one is stopped by the loader before the copy with
    the sentence of the reason. The machine is left to run until the sentence
    and a newline are there, and is then stopped by the test.
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    fd = process.stdout.fileno()
    transcript = b""
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                transcript += chunk
                if b"Micro OS: the image is refused" in transcript and transcript.endswith(
                    b"\n"
                ):
                    break
            if process.poll() is not None:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk
        process.stdout.close()

    return transcript.decode("utf-8", errors="replace")


def run_boot_until(loader: str, kernel: str, needles: list, timeout: float = 10.0) -> str:
    """Boot an image and return the transcript once every needle is there.

    The boot of the open policy continues into the kernel after the warning;
    the test has no disk and no commands, so it stops the machine as soon as
    the kernel says it is up (or the time is out, and the missing line is the
    failure).
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    fd = process.stdout.fileno()
    transcript = b""
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                transcript += chunk
                text = transcript.decode("utf-8", errors="replace")
                if all(needle in text for needle in needles):
                    break
            if process.poll() is not None:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk
        process.stdout.close()

    return transcript.decode("utf-8", errors="replace")


def open_policy_checks(open_build: str) -> list:
    """The other half of ADR 0020: a boot that warns and continues.

    The build of the open policy is the same loader with
    MICRO_OS_REQUIRE_SIGNATURE=OFF. It has to verify the image all the same,
    print the warning and load the kernel; the strict loader of the normal
    build refuses the same bytes in `signature_checks`.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    loader = os.path.join(open_build, "loader32.elf")
    raw = os.path.join(open_build, "microos64.raw")
    signed = os.path.join(open_build, "microos64.bin")

    for path in (loader, raw, signed):
        if not os.path.isfile(path):
            expect("the open-policy build is there", False, path)
            return checks
        expect(f"the open-policy build has {os.path.basename(path)}", True)

    # The image of this build is accepted: the policy only changes what
    # happens to an image that did not pass. The other build has another
    # development key, so its signature would be a different case.
    transcript = run_boot_until(
        loader,
        signed,
        ["Micro OS: image signature ok", "the kernel is up"],
    )
    expect(
        "the open loader accepts a signed image",
        "Micro OS: image signature ok" in transcript and "the kernel is up" in transcript,
        transcript[-300:],
    )

    # A kernel that never had a trailer: the warning, then the kernel.
    transcript = run_boot_until(
        loader,
        raw,
        ["Micro OS: unsigned or damaged image: not guaranteed", "the kernel is up"],
    )
    expect(
        "the open loader warns about an unsigned image",
        "Micro OS: unsigned or damaged image: not guaranteed" in transcript,
        transcript[-300:],
    )
    expect(
        "an unsigned image boots under the open policy",
        "board: qemu_x86_64" in transcript and "the kernel is up" in transcript,
        transcript[-300:],
    )
    expect(
        "an unsigned image never looks verified",
        "Micro OS: image signature ok" not in transcript,
        transcript[-300:],
    )

    # A byte changed in a signed image: the checksum fails, the warning says
    # so, and the policy still boots the bytes that are there.
    with open(signed, "rb") as file:
        damaged = bytearray(file.read())
    damaged[len(damaged) // 2] ^= 0x01
    path = _write_bad_image(bytes(damaged), "open-damaged")
    transcript = run_boot_until(
        loader,
        path,
        ["Micro OS: unsigned or damaged image: not guaranteed", "the kernel is up"],
    )
    expect(
        "the open loader warns about a damaged image",
        "Micro OS: unsigned or damaged image: not guaranteed" in transcript
        and "the checksum does not match" in transcript,
        transcript[-300:],
    )
    expect(
        "a damaged image boots under the open policy",
        "board: qemu_x86_64" in transcript and "the kernel is up" in transcript,
        transcript[-300:],
    )

    return checks


def signature_checks(loader: str, kernel: str) -> list:
    """The trailer of the kernel and the boot that refuses a damaged one.

    The normal run of the smoke test proves the boot accepts the image. These
    runs prove the other half: a byte changed, a trailer removed and a
    signature of another key are each refused by the loader before any byte of
    the kernel runs, and the sentence says which check failed.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    tool = os.path.join(BUILD, "microos_sign")
    public_key = os.path.join(BUILD, "signing.pub")
    for path in (tool, public_key):
        if not os.path.isfile(path):
            expect("the build has the signature tool and its public key", False, path)
            return checks
        expect(f"the build has {os.path.basename(path)}", True)

    try:
        with open(kernel, "rb") as file:
            signed = file.read()
    except OSError as error:
        expect("the signed image is readable", False, str(error))
        return checks

    expect(
        "the kernel carries a signature trailer",
        len(signed) > 120 and signed[-120:-112] == b"MOSIGN01",
    )
    verified = subprocess.run(
        [tool, "verify", "--pub", public_key, kernel],
        capture_output=True,
        text=True,
        check=False,
    )
    expect(
        "the tool verifies the image of the boot",
        verified.returncode == 0,
        verified.stderr.strip() or verified.stdout.strip(),
    )

    # A byte in the middle of the kernel: the SHA-256 refuses it.
    damaged = bytearray(signed)
    damaged[len(damaged) // 2] ^= 0x01
    refused = run_refused_boot(loader, _write_bad_image(bytes(damaged)))
    expect(
        "a damaged kernel is refused before it runs",
        "the image is refused" in refused and "the checksum does not match" in refused,
        refused[-200:],
    )
    expect(
        "the damaged kernel never reached the kernel",
        "the kernel is up" not in refused and "board: qemu_x86_64" not in refused,
        refused[-200:],
    )

    # The trailer removed: the image is a bare binary, not a signed one.
    refused = run_refused_boot(loader, _write_bad_image(signed[:-120]))
    expect(
        "an image without a signature is refused",
        "the image is refused" in refused and "no signature" in refused,
        refused[-200:],
    )

    # The same bytes signed by a key the loader does not carry.
    with tempfile.TemporaryDirectory(prefix="microos-sign-") as folder:
        foreign_key = os.path.join(folder, "foreign.key")
        foreign_image = os.path.join(folder, "foreign.bin")
        made = subprocess.run(
            [tool, "keygen", "--key", foreign_key, "--pub", os.path.join(folder, "foreign.pub")],
            capture_output=True,
            text=True,
            check=False,
        )
        signed_foreign = subprocess.run(
            [
                tool,
                "sign",
                "--key",
                foreign_key,
                "--in",
                os.path.join(BUILD, "microos64.raw"),
                "--out",
                foreign_image,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if made.returncode != 0 or signed_foreign.returncode != 0:
            expect(
                "a second key signs the raw image",
                False,
                (made.stderr + signed_foreign.stderr).strip(),
            )
        else:
            refused = run_refused_boot(loader, foreign_image)
            expect(
                "an image of another key is refused",
                "the image is refused" in refused
                and "the signature does not match" in refused,
                refused[-200:],
            )

    return checks


def _write_bad_image(data: bytes, tag: str = "image") -> str:
    """A temporary kernel image for one refused or warned boot."""
    folder = os.path.join(BUILD, "refused")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"{tag}-{len(data)}.bin")
    with open(path, "wb") as file:
        file.write(data)
    return path


def run_machine(loader: str, kernel: str, image: str, timeout: float, commands: list = None) -> str:
    """Boot the machine, drive the shell and return the transcript.

    The output is read while the machine runs, not after it stops: the
    commands have to arrive after the shell is ready, and the answer of a
    command is the proof the next command may go out.

    The machine has two processors (ADR 0012): the firmware then describes
    both in the MADT of ACPI, and the boot line of the kernel says how many it
    found and how many it runs. The second one is 1 until the AP bring-up is
    written, and the smoke test reads both numbers - a detection that stopped
    working and an AP that was started without the test knowing it are both
    visible here.
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
        "-smp",
        MACHINE_SMP,
        "-nographic",
        "-no-reboot",
        # The disk of the machine: a raw image on the primary IDE channel.
        # The programs of /bin are files of the MicroFS volume in it; the
        # boot writes nothing to the volume (ADR 0008).
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writeback",
    ]

    lines = COMMANDS if commands is None else commands

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    fd = process.stdout.fileno()
    transcript = b""
    sent = 0
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                break  # the machine went away on its own

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            # One command per prompt: the shell prints the prompt before it
            # reads, so the line is written only when the shell is waiting.
            while sent < len(lines) and text.count(PROMPT) > sent:
                process.stdin.write((lines[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            # The prompt after the last command means its answer is done.
            if text.count(PROMPT) > len(lines):
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        # Whatever the machine wrote before it stopped is part of the answer.
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


# The largest image a call of the system reads (K_EXEC_MAX_IMAGE in
# kernel/exec.h). The disk of this machine is 8 MB, so a file past the limit
# needs a volume of its own: the second disk of the smoke is 16 MB and carries
# one file of the limit plus a byte.
K_EXEC_MAX_IMAGE = 8 * 1024 * 1024
OVERSIZE_IMAGE = os.path.join(BUILD, "oversize.img")
OVERSIZE_FILLER = os.path.join(BUILD, "oversize.bin")


def build_oversize_image() -> str:
    """A 16 MB disk with one file of K_EXEC_MAX_IMAGE + 1 zero bytes.

    The volume is written once and kept: it carries the programs of the build
    too, but this boot runs only the big file and the shell, so a rebuild of
    the kernel does not make it stale, and the 8 MiB of blocks are the slowest
    part of the smoke test.
    """
    if os.path.isfile(OVERSIZE_IMAGE):
        return OVERSIZE_IMAGE

    with open(OVERSIZE_FILLER, "wb") as file:
        chunk = b"\0" * (1024 * 1024)
        written = 0
        while written <= K_EXEC_MAX_IMAGE:
            file.write(chunk)
            written += len(chunk)
    os.truncate(OVERSIZE_FILLER, K_EXEC_MAX_IMAGE + 1)

    tool = os.path.join(ROOT, "tools", "x86_image.py")
    result = subprocess.run(
        [
            sys.executable,
            tool,
            "--image",
            OVERSIZE_IMAGE,
            "--size-mb",
            "16",
            "--program",
            f"/big.bin={OVERSIZE_FILLER}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stdout + result.stderr).strip())
    return OVERSIZE_IMAGE


def oversize_checks(loader: str, kernel: str, timeout: float) -> list:
    """One boot of a disk with an image past K_EXEC_MAX_IMAGE on it.

    The call refuses the file before a byte is read and the answer is E2BIG,
    which the shell prints as "argument list too long"; the descriptor of the
    caller is not spent, and the shell answers the next command.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    try:
        image = build_oversize_image()
    except (OSError, RuntimeError) as error:
        expect("the disk of the oversize image is built", False, str(error))
        return checks

    expect("the disk of the oversize image is built", os.path.isfile(image), image)

    output = run_machine(
        loader,
        kernel,
        image,
        timeout,
        commands=["exec /big.bin", "uname"],
    )

    expect("the machine boots on the oversize disk", PROMPT in output, output[-300:])
    expect(
        "an image past K_EXEC_MAX_IMAGE is E2BIG",
        "exec: /big.bin: argument list too long" in output,
        output[-500:],
    )
    expect(
        "the refused image spends no descriptor and the shell lives",
        re.search(r"MicroOS \d+\.\d+\.\d+", output) is not None,
        output[-300:],
    )

    return checks


def check(output: str) -> list:
    """The expectations about the boot and the shell of the machine."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the boot
    for line in EXPECTED:
        expect(line, line in output)

    # --- the program of the boot
    # The line in the middle is not written by the kernel: it comes out of the
    # image that was read from /bin/hello of the disk. The lines after it are
    # answers of the kernel to its calls - a pid, a time and a status that the
    # program could not invent - so the transcript shows the door and the way
    # back, not only a string.
    expect(
        "the volume of the machine is the disk",
        "x86-64: the root volume is ata0p1" in output,
        "the machine booted without the image of the disk",
    )
    expect(
        "the boot finds the programs on the disk",
        "x86-64: the programs come from ata0p1:/bin" in output,
    )
    # Nothing is written to the volume at the boot: the program is a file the
    # tool left there, not a blob of the kernel that was installed first.
    expect(
        "the boot does not install the programs",
        "is written" not in output,
        "a line of the boot wrote a file of /bin",
    )

    # --- the processors of the machine (ADR 0012)
    expect(
        "the firmware lists both processors of the machine",
        re.search(r"x86-64: 2 cpu\(s\) reported by the firmware", output) is not None,
        "the MADT of ACPI was not read, or QEMU was not given -smp 2",
    )
    expect(
        "the kernel runs on the cores it has started",
        re.search(r"reported by the firmware, [12] in use", output) is not None,
    )
    expect(
        "the boot names the second core as not started",
        "the AP bring-up is not written yet" in output
        if "reported by the firmware, 1 in use" in output
        else True,
        "the kernel says it uses the second core without the AP bring-up",
    )
    expect(
        "the boot asks for the program through the call",
        "x86-64: running /bin/hello through the call" in output,
    )
    expect("the program writes to the console", "PROGRAM: hello from /bin" in output)
    # The number is read from CS by the program itself: it is the proof that
    # the entry went through an iretq into ring 3 and not into a function.
    expect("the program runs in ring 3", "PROGRAM: cpl 3" in output)
    # No arguments at the boot: the port is entered without a vector.
    expect("the program starts without arguments", "PROGRAM: argc 0" in output)
    # The environment the kernel fills in when the call carries none: the
    # entry ABI has argc, argv and envp in RDI, RSI and RDX (ADR 0016), and a
    # program that prints the vector proves the third register crossed too.
    expect(
        "the program starts with the environment of the system",
        re.search(r"PROGRAM: envp \d+, PATH=/bin", output) is not None,
    )
    expect(
        "the program asks the kernel for its pid and the time",
        re.search(r"PROGRAM: pid \d+, up \d+ ms", output) is not None,
    )
    # The sleep is the call that proves the interrupts of the stub: the clock
    # of the kernel has moved by the time the program is back on its feet.
    expect(
        "the program sleeps inside a call",
        re.search(r"PROGRAM: slept 10 ms, up \d+ ms", output) is not None,
    )
    expect("the program returns a status", "x86-64: the program exited with 0" in output)
    expect("the kernel is back from the program", "x86-64: the program returned" in output)

    # --- the shell
    expect("the shell starts", "MicroOS shell ready" in output)
    expect(
        "the shell runs in a task",
        "no tasks yet" not in output,
        "the task substrate of the port is not there",
    )

    # The answers, separated by the prompt. The echo of the typed line is
    # part of the block; the checks below look for the answer itself.
    blocks = output.split(PROMPT)[1:]

    def answer(command: str, occurrence: int = 0) -> str:
        """The answer of one command of the list, empty when it did not run.

        A command may be typed twice to see the two sides of a rule - the
        second exec of the file comes after the mode is given back - so the
        occurrence of the line decides which block is the answer.
        """
        indexes = [index for index, line in enumerate(COMMANDS) if line == command]
        if occurrence >= len(indexes):
            return ""
        index = indexes[occurrence]
        return blocks[index] if index < len(blocks) else ""

    expect(
        "every command is answered",
        len(blocks) > len(COMMANDS),
        f"{len(blocks)} block(s) for {len(COMMANDS)} command(s)",
    )

    expect("help lists the commands", "list the available commands" in answer("help"))
    # The shell command of the signal: a pid no task has is a sentence, and
    # the shell of the machine is alive after it.
    expect(
        "kill of a pid that is not there is reported",
        "kill: 9999: no such process" in answer("kill 9999"),
    )
    # The version is whatever the build says: the shape is checked, so a
    # release does not break the smoke test.
    expect(
        "uname prints the name and the version",
        re.search(r"MicroOS \d+\.\d+\.\d+", answer("uname")) is not None,
    )
    expect(
        "uptime prints the time",
        re.search(r"up \d+ s", answer("uptime")) is not None,
    )
    # The limits of the shell come from the kernel: the memory, the CPU time
    # and the descriptors, at the ceiling of the system after the boot.
    limits = answer("ulimit")
    expect(
        "ulimit prints the limits of the task",
        all(word in limits for word in ("memory", "cpu", "fds")),
        limits,
    )
    # The volume is not part of the kernel image: the files were written by
    # the tool of the build, so a listing is the proof that the bytes are on a
    # filesystem and not a line the kernel printed about itself.
    bin_listing = answer("ls /bin")
    expect("the program is a file in /bin", "hello" in bin_listing)
    # Every program of the port is an object of the volume the boot mounted,
    # with the mode the tool gave it; a missing one is a tool or a build that
    # lost it.
    stat_answer = answer("stat /etc/setting")
    expect(
        "stat prints the mode in octal",
        re.search(r"mode: 0[0-7]{3}", stat_answer) is not None,
        stat_answer,
    )
    umount_answer = answer("umount /")
    expect(
        "the root of the tree is not unmounted",
        "is the root of the tree" in umount_answer,
        umount_answer,
    )
    expect(
        "a filesystem of the kernel is not unmounted",
        "/proc is mounted by the kernel" in answer("umount /proc"),
        answer("umount /proc"),
    )
    expect(
        "df refuses a path that is not there",
        "df: /nope: no such file or directory" in answer("df /nope"),
        answer("df /nope"),
    )
    expect(
        "path refuses the empty value",
        "an empty value is not a list of directories" in answer('path ""'),
        answer('path ""'),
    )
    expect("the tree is still there", "etc" in answer("ls /"), answer("ls /"))

    # --- the tools of sbase (third_party/sbase) are the commands of /bin
    #
    # The shell looks in PATH before its own table for these ten names, so a
    # bare `ls` is the program of the disk (the kernel says `task: created
    # 'program'`) and not the command of the shell. The checks below read the
    # answer of the program and the escape of the colour.
    listing = answer("ls -la /etc")
    expect(
        "ls -la prints the mode, the owner and the group",
        re.search(r"-rw-r--r--\s+1 root\s+root\s+34 Jan 01 00:00 setting", listing) is not None,
        listing,
    )
    expect(
        "ls -la lists the directory the image has",
        "init.d" in listing and "licenses" in listing,
        listing,
    )
    expect(
        "a bare command is the program of /bin, not the command of the shell",
        "task: created 'program'" in listing,
        listing,
    )
    found = answer("find /etc -name setting")
    expect("find walks a tree and matches a name", "/etc/setting" in found, found)
    ordered = answer("sort /etc/setting")
    expect(
        "sort orders the lines",
        ordered.index("console=vga") < ordered.index("hello") < ordered.index("hostname=x86_64"),
        ordered,
    )
    recursive = answer("grep -r hostname /etc")
    expect(
        "grep -r names the file of a line",
        "/etc/setting:hostname=x86_64" in recursive,
        recursive,
    )
    recursive = answer("grep -r -i VGA /etc")
    expect("grep -r -i folds the case", "console=vga" in recursive, recursive)
    count = answer("wc -l /etc/setting")
    expect("wc counts the lines", "3 /etc/setting" in count, count)
    last = answer("tail -n 1 /etc/setting")
    expect("tail prints the last line", "console=vga" in last, last)
    first_line = answer("head -n 1 /etc/setting")
    expect("head prints the first line", "hello" in first_line, first_line)
    changed = answer("sed -n s/x86_64/X/p /etc/setting")
    expect("sed matches and substitutes a line", "hostname=X" in changed, changed)
    text = answer("cat /etc/setting")
    expect("cat copies a file", "hostname=x86_64" in text and "console=vga" in text, text)
    piped = answer("ls /etc | xargs -n 4")
    expect(
        "xargs runs a command over the words of a pipe",
        "setting" in piped and "licenses" in piped,
        piped,
    )
    # The console of the serial copy is not a terminal: the SGR sequence of
    # the colour is consumed by the console and never reaches this text. The
    # colour itself is checked out of the picture by `make x86-vga-smoke`;
    # here the check is that the flag is accepted and the names are printed.
    coloured = answer("ls --color /etc")
    expect(
        "ls --color prints the names of the directory",
        all(name in coloured for name in ("init.d", "licenses", "setting")),
        coloured,
    )
    plain = answer("ls --color=never /etc")
    expect("ls --color=never writes no escape", "\x1b[" not in plain, plain)
    explicit = answer("exec /bin/ls -la /etc")
    expect("the file of the tool is /bin/ls", "setting" in explicit and "root" in explicit, explicit)
    explicit = answer("exec /bin/find /etc -name setting")
    expect("the file of the tool is /bin/find", "/etc/setting" in explicit, explicit)
    free_answer = answer("free")
    expect(
        "free prints the memory and not the clock",
        "heap:" in free_answer and "uptime:" not in free_answer,
        free_answer,
    )
    # The clock of this port moves by the millisecond: a single 32 KB fill
    # used to divide by one microsecond and claim tens of gigabytes per
    # second. The measurement repeats the operation, and every speed is a
    # number a machine of this kind could give, not a division by a still
    # clock. The bound is generous (16 GB/s) and still below the artifact of
    # the old code (32 GB/s for one 32 KB memset in one microsecond).
    measured = answer("memtest")
    speeds = [int(value) for value in re.findall(r"(\d+) KB/s", measured)]
    expect(
        "memtest measures and keeps the data",
        " KB block: fill " in measured and "data ok" in measured,
        measured,
    )
    expect("memtest prints at least one speed", len(speeds) > 0, measured)
    expect(
        "memtest speeds are not divided by a still clock",
        all(value < 16000000 for value in speeds),
        measured,
    )

    # --- `.` and `..` in a path
    # The shell joins the path with the current directory and normalizes it
    # before the call: the filesystem of this system never sees a `..`.
    expect("cd into a directory works", "/etc" in answer("pwd"), answer("pwd"))
    expect("cd . stays in the directory", "/etc" in answer("pwd", 1), answer("pwd", 1))
    expect(
        "repeated and trailing slashes are one path",
        "/etc" in answer("pwd", 2),
        answer("pwd", 2),
    )
    expect("cd .. goes to the parent", re.search(r"(?m)^/\s*$", answer("pwd", 3)) is not None,
           answer("pwd", 3))
    parent = answer("ls ..")
    expect(
        "ls .. lists the parent",
        all(re.search(rf"(?m)^{name}\r?$", parent) is not None for name in ("bin", "etc")),
        parent,
    )

    # --- a directory wider than the old table
    many = answer("ls /many")
    expect(
        "ls lists a directory past 64 names",
        len(re.findall(r"(?m)^f\d\d\r?$", many)) == MANY_FILES,
        many,
    )
    expect("ls reaches the last name of a wide directory", "f69" in many, many)
    wide_tree = answer("tree /many")
    expect(
        "tree walks a directory past 16 names and counts it whole",
        "0 directories, 70 files" in wide_tree
        and wide_tree.count("|-- ") + wide_tree.count("`-- ") == MANY_FILES,
        wide_tree,
    )
    expect(
        "the programs of the port are files of the disk",
        all(
            name in bin_listing
            for name in (
                "hello",
                "crash",
                "fork",
                "badptr",
                "signals",
                "nx",
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
                "zcat",
                "pcre2grep",
                "lua",
                "tail",
                "sort",
                "find",
                "xargs",
                "sed",
                "bc",
                "dc",
            )
        ),
        bin_listing[-200:],
    )
    # /etc/setting came with the image: the shell reads it through the same
    # filesystem and no command of this run wrote it.
    expect(
        "the setting is a file of the disk",
        "hello" in answer("cat /etc/setting"),
        answer("cat /etc/setting"),
    )
    # The utilities of the library are files of /bin like the first four: the
    # listing of the shell is the proof the tool wrote every image.
    expect(
        "the utilities are files in /bin",
        all(name in answer("ls /bin") for name in ("wc", "head", "grep", "mkdir", "rm", "pwd")),
    )
    # The same file, asked for with a command of the shell: the library, the
    # syscall, the mode of the record and the jump are the ones of a user and
    # not the ones of the boot.
    expect(
        "the shell runs the program through execve",
        "the program has finished" in answer("exec /bin/hello", 1),
    )
    expect(
        "the program of the shell writes to the console",
        "PROGRAM: hello from /bin" in answer("exec /bin/hello", 1),
    )
    # The status of a program of the shell comes back the same way: the port
    # reports it, the kernel hands it to the call, and the call ends with 0
    # because there is no process to exit into yet. The shell is told that the
    # program finished and not that the machine died.
    expect(
        "the program of the shell returns its status",
        "the program exited with 0" in answer("exec /bin/hello", 1),
    )
    expect(
        "the shell sees the program finish",
        "the program has finished" in answer("exec /bin/hello", 1),
    )
    # The arguments of a line of the shell reach the program: the name of the
    # program first, then the words of the line; and the environment the
    # shell did not pass is the one of the system.
    expect(
        "the arguments of the shell reach the program",
        "PROGRAM: argc 3, argv[0] /bin/hello" in answer("exec /bin/hello one two"),
    )
    expect(
        "the program of the shell sees the environment",
        re.search(r"PROGRAM: envp \d+, PATH=/bin", answer("exec /bin/hello one two")) is not None,
    )

    # --- the programs of the library (ADR 0016)
    # cat opens a file the shell wrote, reads it and writes the bytes to the
    # console: the call of the file, of the buffer and of the console in one
    # image that was linked against components/libc/user and not against
    # programs/prog.h.
    cat = answer("exec /bin/cat /etc/setting")
    expect("cat reads a file through the library", "hello" in cat)
    expect("cat is a program of the shell", "the program has finished" in cat)
    # The error path is the formatter of the library (%s and %d) and the
    # status of a program that did not succeed: 1, the code of the shell
    # command, and not the 255 of a fault.
    cat = answer("exec /bin/cat /no/such/file")
    expect(
        "cat reports a file the formatter prints",
        "/no/such/file: no such file or directory" in cat,
        cat,
    )
    expect("a program returns the status the library gives it", "the program exited with 1" in cat)
    # echo is the vector of the arguments and the formatter of the library.
    echo = answer("exec /bin/echo one two")
    expect("echo writes its arguments", "one two" in echo)
    expect("echo is a program of the shell", "the program has finished" in echo)
    # ls is a directory through readdir, which is a descriptor with a
    # position of its own: the names of /bin come back in one line.
    listing = answer("exec /bin/ls /bin")
    expect(
        "ls reads a directory through the library",
        "hello" in listing and "cat" in listing and "echo" in listing and "ls" in listing,
    )
    expect("ls is a program of the shell", "the program has finished" in listing)

    # --- a machine without a store (this port has none yet)
    # The command must not print an empty answer as if the store had no
    # keys, and `config sync` must not report "0 saved" as success: the
    # files of /etc are the settings of this machine.
    no_store = answer("config")
    expect(
        "config says this machine has no store",
        "no store" in no_store and "files of /etc are the settings" in no_store,
        no_store,
    )
    no_sync = answer("config sync")
    expect(
        "config sync explains the missing store",
        "no store" in no_sync and "files of /etc are the settings" in no_sync,
        no_sync,
    )
    expect(
        "config sync does not report a false save",
        not re.search(r"\d+ file\(s\) restored, \d+ saved", no_sync),
        no_sync,
    )

    # --- the systems of /etc (ADR 0021)
    # The boot wrote the three files of the tree on the volume; `config list`
    # names them and says which one this machine applies. `display` is the
    # panel of the board: the key is written and kept, and the machine says it
    # does not apply it. `font` is applied by this machine for its line gap
    # (tools/x86_vga_smoke.py reads the rows and the step of a line out of the
    # picture); `scale`, `columns` and `rows` are the cap of the panel and are
    # kept with a sentence.
    systems = answer("config list")
    expect(
        "config list names the systems of the tree",
        all(name in systems for name in ("display", "font", "console")),
        systems,
    )
    expect(
        "config list says which system this machine applies",
        "display" in systems
        and "/etc/display" in systems
        and "(not applied here)" in systems,
        systems,
    )
    # The value of the last save of the defaults, read back through the same
    # file: a key that was never set is the default of the tree.
    expect(
        "config get reads the default of a system",
        "display.rotation=0" in answer("config get display.rotation"),
        answer("config get display.rotation"),
    )
    # The console is the system of this machine: the theme repaints the
    # screen, and the log line names the pair and the screen it was put on.
    # The screen of this machine is the framebuffer of ADR 0023; the text mode
    # of a machine without the interface is checked by tools/x86_vga_smoke.py,
    # which reads the attribute of every cell there.
    themed = answer("config set console.theme light")
    expect("config set applies the console", "console.theme=light" in themed, themed)
    expect(
        "the theme of the console reaches the screen",
        "console: theme light, invert 0, the screen the framebuffer of 1280x800" in themed,
        themed,
    )
    expect(
        "config get reads the key that was set",
        "console.theme=light" in answer("config get console.theme"),
        answer("config get console.theme"),
    )
    # A system of the board on this machine: the file is written and the
    # answer says the value is kept, which is what the ADR asks of a machine
    # without the hardware.
    kept = answer("config set display.invert 0")
    expect(
        "a system this machine cannot honour is kept and reported",
        "display.invert=0" in kept and "kept" in kept,
        kept,
    )
    expect(
        "the key that was kept is read back",
        "display.invert=0" in answer("config get display.invert"),
        answer("config get display.invert"),
    )
    # The file of the system is text of the volume, whole: the comment of the
    # defaults and the line that `config set` replaced are both there.
    display_file = answer("cat /etc/display")
    expect(
        "the system file is the text of the volume",
        "# the screen" in display_file and "invert=0" in display_file,
        display_file,
    )


    root_listing = answer("ls /")
    expect(
        "ls / lists the names",
        all(name in root_listing for name in ("bin", "etc", "proc", "dev")),
        root_listing,
    )
    expect("ls is not a tree", "`--" not in root_listing and "|--" not in root_listing)
    tree_listing = answer("tree /")
    expect("tree / draws the branches", "|-- " in tree_listing and "`-- " in tree_listing)
    expect(
        "tree / shows the tree of the machine",
        all(name in tree_listing for name in ("bin/", "etc/", "proc/", "dev/")),
        tree_listing,
    )
    expect(
        "tree / counts the directories and the files",
        re.search(r"\d+ director(?:y|ies), \d+ files?", tree_listing) is not None,
    )
    # `-d` draws the directories and counts every entry: the summary of the
    # two runs is the same, and no file of the tree is drawn by `-d`.
    tree_d_listing = answer("tree -d /")
    full_counts = re.search(r"(\d+) director(?:y|ies), (\d+) files?", tree_listing)
    dirs_counts = re.search(r"(\d+) director(?:y|ies), (\d+) files?", tree_d_listing)
    expect(
        "tree -d draws the directories only",
        "|-- " in tree_d_listing and "setting" not in tree_d_listing,
        tree_d_listing,
    )
    expect(
        "tree -d counts the same files as tree",
        full_counts is not None
        and dirs_counts is not None
        and full_counts.groups() == dirs_counts.groups(),
        f"{dirs_counts.groups() if dirs_counts else tree_d_listing}"
        f" against {full_counts.groups() if full_counts else tree_listing}",
    )
    expect(
        "tree -d counts the files it did not draw",
        dirs_counts is not None and int(dirs_counts.group(2)) > 0,
        tree_d_listing,
    )
    # --- the virtual filesystems of the tree
    # The board and the simulator mount procfs and devfs at the boot; the
    # x86-64 machine does the same, so the wiki's `tree /` is true here too.
    proc_listing = answer("ls /proc")
    expect(
        "ls /proc shows the information of the system",
        all(name in proc_listing for name in ("version", "uptime", "meminfo")),
        proc_listing,
    )
    version = answer("cat /proc/version")
    expect(
        "cat /proc/version reads the file of the kernel",
        re.search(r"MicroOS \d+\.\d+\.\d+", version) is not None,
        version,
    )
    dev_listing = answer("ls /dev")
    expect(
        "ls /dev shows the console and the sink",
        "tty0" in dev_listing and "null" in dev_listing,
        dev_listing,
    )

    # --- the utilities of the library: wc, head, grep, mkdir, rm, pwd
    # The file the image carries is "hello\nhostname=x86_64\nconsole=vga\n":
    # three lines and 34 bytes. The numbers are of the file, and a program
    # that read its own arguments could not produce them.
    wc = answer("exec /bin/wc /etc/setting")
    expect("wc counts the lines, words and bytes", "3 3 34 /etc/setting" in wc)
    expect("wc counts what it read", "the program has finished" in wc)
    wc = answer("exec /bin/wc -l /etc/setting")
    expect("wc -l is a count of the newlines", "3 /etc/setting" in wc)
    wc = answer("exec /bin/wc -c /etc/setting")
    expect("wc -c is a count of the bytes", "34 /etc/setting" in wc)
    # head copies the first line of the file and stops: the file has one, so
    # the block holds it once and the program has a status of 0.
    head = answer("exec /bin/head -n 1 /etc/setting")
    expect("head prints the first line", "hello" in head)
    expect("head is a program of the shell", "the program has finished" in head)
    expect("head returns its status", "the program exited with 0" in head)
    # grep searches a line and prints it; -i folds the case of the word.
    grep = answer("exec /bin/grep hello /etc/setting")
    expect("grep prints a line that holds the word", "hello" in grep)
    expect("grep is a program of the shell", "the program has finished" in grep)
    grep = answer("exec /bin/grep -i HELLO /etc/setting")
    expect("grep -i folds the case", "hello" in grep)
    # PCRE2 is a library and not a word search: `.` is one character, `^` and
    # `$` are the ends of the line, and a group in parentheses is numbered.
    # The output of -o with -g 1 is the group alone; the pattern itself is in
    # the echo of the typed line and not in the answer.
    pcre = answer("exec /bin/pcre2grep -i h.LLO /etc/setting")
    expect(
        "pcre2grep matches a line with a wildcard and folds the case",
        any(line.strip() == "hello" for line in pcre.splitlines()),
        pcre[-120:],
    )
    pcre = answer("exec /bin/pcre2grep -o -g 1 ^hostname=(x86_64)$ /etc/setting")
    expect(
        "pcre2grep prints the group of an anchored pattern",
        any(line.strip() == "x86_64" for line in pcre.splitlines()),
        pcre[-120:],
    )
    licenses = answer("exec /bin/ls /etc/licenses")
    expect("the license of PCRE2 is a file of the disk", "pcre2" in licenses, licenses)
    # No file is descriptor 0. A program inherits the table of the shell
    # (ADR 0017), but the prompt does not open the console on descriptor 0
    # yet, so the read answers EBADF; a redirect or a pipe fills it.
    cat = answer("exec /bin/cat")
    expect(
        "cat reads descriptor 0 when no file is named",
        "bad file descriptor" in cat,
        cat,
    )
    expect("cat of a closed stdin is a failure", "the program exited with 1" in cat)
    wc = answer("exec /bin/wc -l -")
    expect("wc reads descriptor 0 for a dash", "bad file descriptor" in wc, wc)
    # The tree: the shell makes /tmp, the program makes a directory in it.
    # Without -r a directory is refused, even an empty one; with -r the tree
    # goes away and the listing of it is ENOENT (2).
    made = answer("exec /bin/mkdir /tmp/d")
    expect("mkdir makes a directory through the call", "mkdir:" not in made)
    expect("mkdir returns its status", "the program exited with 0" in made)
    listing = answer("exec /bin/ls /tmp")
    expect("the directory made by the program is there", re.search(r"(?m)^d\s*$", listing) is not None)
    refused = answer("exec /bin/rm /tmp/d")
    expect("rm refuses a directory without -r", "is a directory, use -r" in refused)
    removed = answer("exec /bin/rm -r /tmp")
    expect("rm -r takes a tree down", "rm:" not in removed)
    expect("rm -r returns its status", "the program exited with 0" in removed)
    gone = answer("exec /bin/ls /tmp", 1)
    expect("the tree is gone", "ls: lstat /tmp: no such file or directory" in gone, gone)
    # pwd is the directory of the task, read through the call: the shell
    # starts at /, and the block after the typed line is that one character.
    pwd = answer("exec /bin/pwd")
    expect("pwd prints the directory of the task", re.search(r"(?m)^/\r?$", pwd) is not None)
    expect("pwd is a program of the shell", "the program exited with 0" in pwd)

    # --- the first port of somebody else's program: zlib and zcat
    # The .gz is a fixture of the image and the text below is its content.
    # `wc -c` is the count of the bytes the library inflated, and `cat` their
    # text: a stream that stopped one byte short would fail one of the two.
    zcat = answer("exec /bin/zcat -o /tmp/greeting.txt /etc/greeting.gz")
    expect("zcat is a program of the shell", "the program exited with 0" in zcat, zcat)
    expect("zcat does not report a stream it read", "zcat:" not in zcat, zcat)
    count = answer("exec /bin/wc -c /tmp/greeting.txt")
    expect("zcat writes every inflated byte", "134 /tmp/greeting.txt" in count, count)
    text = answer("exec /bin/cat /tmp/greeting.txt")
    expect(
        "the inflated bytes are the fixture",
        "Micro OS: the first port of a program" in text
        and "zlib 1.3.1, MicroFS and ring 3" in text
        and "the bytes of this file were inflated by the library of the guest" in text,
        text,
    )
    test = answer("exec /bin/zcat -t /etc/greeting.gz")
    expect(
        "zcat -t checks the stream and writes nothing",
        "the program exited with 0" in test and "damaged" not in test and "Micro OS" not in test,
        test,
    )
    verbose = answer("exec /bin/zcat -v -t /etc/greeting.gz")
    expect(
        "zcat -v prints the count of the inflated bytes",
        "zcat: /etc/greeting.gz: 134 bytes" in verbose and "the program exited with 0" in verbose,
        verbose,
    )
    missing = answer("exec /bin/zcat /etc/no-such.gz")
    expect(
        "zcat reports a file that is not there",
        "zcat: /etc/no-such.gz: no such file or directory" in missing
        and "the program exited with 1" in missing,
        missing,
    )
    piped = answer("cat /etc/greeting.gz | /bin/zcat")
    expect(
        "zcat reads a gzip stream from a pipe",
        "Micro OS: the first port of a program" in piped
        and "the bytes of this file were inflated by the library of the guest" in piped,
        piped,
    )
    licenses = answer("exec /bin/ls /etc/licenses")
    expect("the license of zlib travels with the image", "zlib" in licenses, licenses)
    expect("the license of sbase travels with the image", "sbase" in licenses, licenses)

    # --- the second port of somebody else's program: Lua and /bin/lua
    # The script and the license are files of the image, the interpreter is a
    # file of /bin, and every line below is the library of the guest computing
    # in a double: a power, the constant of pi, three places of a fraction,
    # the two values that are not numbers, the clock, and a script that reads
    # and writes a file of the VFS.
    lua_script = answer("exec /bin/cat /etc/hello.lua")
    expect(
        "the script of the first Lua program is a file of the image",
        "the first script of the machine" in lua_script,
        lua_script[:200],
    )
    expect("the license of Lua travels with the image", "lua" in licenses, licenses)

    lua_power = answer("exec /bin/lua -e print(2^10)")
    expect(
        "lua -e prints a power of a double",
        "1024.0" in lua_power and "the program exited with 0" in lua_power,
        lua_power,
    )
    lua_pi = answer("exec /bin/lua -e print(math.pi)")
    expect("lua prints the constant of pi", "3.1415926535898" in lua_pi, lua_pi)
    lua_format = answer('exec /bin/lua -e print(string.format("%.3f",1/3))')
    expect(
        "string.format writes three places of a fraction",
        "0.333" in lua_format and "the program exited with 0" in lua_format,
        lua_format,
    )
    lua_special = answer("exec /bin/lua -e print(1/0,0/0)")
    expect(
        "the values that are not numbers are words",
        "inf" in lua_special and "nan" in lua_special,
        lua_special,
    )
    lua_clock = answer("exec /bin/lua -e print(os.time()>=0)")
    expect("os.time answers a number of seconds", "true" in lua_clock, lua_clock)

    lua_run = answer("exec /bin/lua /etc/hello.lua")
    expect(
        "the script of /etc runs in the interpreter",
        all(
            line in lua_run
            for line in (
                "lua: hello from /etc/hello.lua",
                "lua: table\t3\talpha\tgamma",
                "lua: pi 3.142, 2^10 1024",
                "lua: read\ta line written by lua",
                "lua: os.time\ttrue",
                "lua: string\tABC\txxx",
            )
        )
        and "the program exited with 0" in lua_run,
        lua_run,
    )
    lua_missing = answer("exec /bin/lua /etc/no-such.lua")
    expect(
        "a script that is not there is the sentence of the interpreter",
        "cannot open /etc/no-such.lua" in lua_missing and "exited with 1" in lua_missing,
        lua_missing,
    )

    # --- the third port of somebody else's program: bc and dc
    # One library and two `main`s: the name of the program in `argv[0]` picks
    # the engine, and the scripts of /etc are files of the volume. The lines
    # are the numbers of the release: a power of 31 digits, the arctangent of
    # 1 at scale 10 (the math library truncates at the scale of the call, as
    # GNU bc does), a base read and a base written, the square root of the
    # script, a function with a loop, an array and a string.
    bc_cat = answer("exec /bin/cat /etc/bc.bc")
    expect(
        "the script of bc is a file of the image",
        "the script of the machine" in bc_cat,
        bc_cat[:200],
    )
    expect(
        "the license of bc travels with the two calculators",
        "bc" in licenses,
        licenses,
    )

    bc_power = answer("exec /bin/bc -e 2^100")
    expect(
        "bc prints a power of thirty-one digits",
        "1267650600228229401496703205376" in bc_power
        and "the program exited with 0" in bc_power,
        bc_power,
    )
    bc_pi = answer("exec /bin/bc -l -e scale=10 -e 4*a(1)")
    expect(
        "bc -l computes the arctangent of 1 at scale 10",
        "3.1415926532" in bc_pi and "the program exited with 0" in bc_pi,
        bc_pi,
    )
    bc_base = answer("exec /bin/bc -e ibase=16 -e FF")
    expect("bc reads a number in base 16", "255" in bc_base, bc_base)
    bc_obase = answer("exec /bin/bc -e obase=2 -e 10")
    expect("bc writes a number in base 2", "1010" in bc_obase, bc_obase)

    bc_run = answer("exec /bin/bc -l -f /etc/bc.bc < /dev/null")
    expect(
        "the script of /etc runs in the calculator",
        all(
            line in bc_run
            for line in (
                "1267650600228229401496703205376",
                "1.41421356237309504880",
                "255",
                "1010",
                "55",
                "the sum is right",
                "3",
                "42",
                "3.1415926532",
            )
        )
        and "the sum is right\r\n3\r\n42\r\n" in bc_run
        and "the program exited with 0" in bc_run,
        bc_run,
    )
    bc_stdin = answer("exec /bin/bc -l < /etc/bc.bc")
    expect(
        "bc reads its script from descriptor 0",
        "the sum is right" in bc_stdin and "the program exited with 0" in bc_stdin,
        bc_stdin,
    )

    dc_run = answer("exec /bin/dc /etc/dc.dc")
    expect(
        "dc is the same engine in RPN",
        all(line in dc_run for line in ("5", "2", ".33333", "1024"))
        and "the program exited with 0" in dc_run,
        dc_run,
    )
    dc_sum = answer("exec /bin/dc -e 2 -e 3+p")
    expect("dc adds two numbers on its stack", "5" in dc_sum, dc_sum)

    # A program starts another program with an environment of its own. The
    # two words are the vector of `spawn.c`, copied by the kernel at the edge
    # and placed in the memory of the second program: the entry prints them.
    spawn = answer("exec /bin/spawn")
    expect(
        "a program starts another with its own environment",
        "PROGRAM: envp 2, PATH=/custom" in spawn,
    )
    expect("the started program gets its arguments", "PROGRAM: argc 2, argv[0] hello" in spawn)
    expect("the outer program waits for the inner one", "the program has finished" in spawn)

    # --- the pipes and the redirects of K7 (ADR 0017)
    # `echo hi > /tmp/f` is a builtin that writes descriptor 1, which the
    # stage put on the file; `cat /tmp/f` reads it back, so the bytes went
    # through the filesystem and not through the buffer of the shell.
    expect("a redirect writes the file", "hi" in answer("cat /tmp/f"))
    expect("a redirect reads the file", "hi" in answer("cat < /tmp/f"))
    # Two builtins in a pipe: the consumer prints what the producer wrote.
    expect("a pipe joins two commands of the shell", "one" in answer("echo one | cat"))
    # A builtin and a program: the stage of /bin/cat inherits descriptor 0
    # from the shell, which is the read end of the pipe.
    expect("a program reads the stdin of a pipe", "one" in answer("echo one | /bin/cat"))
    # A program of the machine counts the lines of the shell's command: the
    # setting has three, and the program read them through the pipe.
    line_count = answer("cat /etc/setting | wc -l")
    expect(
        "wc counts the lines of a pipe",
        re.search(r"\n3[ \t]*\r?\n", line_count) is not None,
        repr(line_count[-40:]),
    )
    bin_count = answer("ls /bin | wc -l")
    match = re.search(r"\n(\d+)[ \t]*\r?\n", bin_count)
    expect(
        "a listing of the shell is counted by a program",
        match is not None and int(match.group(1)) > 3,
        repr(bin_count[-40:]),
    )
    # A command that changes the task of the shell cannot be a stage.
    expect(
        "cd in a pipeline is refused with a sentence",
        "cannot run in a pipeline" in answer("cd / | cat"),
    )
    expect(
        "a file that is not there is the failure of one stage",
        "no such file" in answer("cat /no/such/file | /bin/cat"),
    )
    # The mode is a fact of the record and not a word of the kernel: take the
    # bit away with the shell and the same call refuses, give it back and the
    # program runs again. The library writes its errno through %fs here, so a
    # machine without a thread pointer would fault before it answered.
    expect(
        "a file without the x bit is not a program",
        "has no execute bit" in answer("exec /bin/hello", 0),
    )
    expect(
        "the owner is a number of the shell",
        "chown:" not in answer("chown 0 /bin/hello"),
    )
    # The volume the boot wrote and the shell read is checked by the
    # filesystem itself, and the checker is on the machine, not in the test.
    expect("the volume is consistent", "the volume is consistent" in answer("fsck"))

    # --- the fork: an address space per task and copy on write
    fork = answer("exec /bin/fork")
    expect("the forking program starts", "PROGRAM: fork starts, value 42" in fork)
    # The child gets the zero of the fork and writes the page it shares with
    # its parent; the write faults and the handler copies the page.
    expect(
        "the child of the fork gets zero and its own page",
        "PROGRAM: child fork returned 0, value 99" in fork,
    )
    expect(
        "the parent gets the pid of the child",
        re.search(r"PROGRAM: parent fork returned [1-9][0-9]*", fork) is not None,
    )
    # The parent never wrote the page: it still reads the value it had before
    # the fork. Shared memory without the fault of the first write is 99.
    expect("the parent keeps its own page", "PROGRAM: parent sees value 42" in fork)
    expect(
        "the parent waits and gets the status of the child",
        re.search(r"PROGRAM: waited, status 7", fork) is not None,
    )
    expect("the fork program returns its status", "the program exited with 0" in fork)
    expect("the shell sees the fork program finish", "the program has finished" in fork)

    # The second run of the same program comes after the tables and frames of
    # the first were released; a space that leaked would run out by now.
    fork = answer("exec /bin/fork", 1)
    expect(
        "the fork runs again with the same answer",
        "PROGRAM: parent sees value 42" in fork
        and "PROGRAM: waited, status 7" in fork
        and "the program exited with 0" in fork,
    )

    # --- signals: a handler, SIG_DFL, SIG_IGN and a wait cut short
    signals = answer("exec /bin/signals")
    expect("the signals program starts", "PROGRAM: signals starts, pid" in signals)
    # The handler is the code of the program, entered by the kernel on the
    # stack of the program and returned from through the restorer page.
    expect(
        "a handler of the program runs for its own SIGINT",
        "PROGRAM: sigint handler, signal 2" in signals,
    )
    expect(
        "the system call of the handler returns its answer",
        "PROGRAM: self SIGINT returned 0, handler runs 1" in signals,
    )
    # SIG_IGN is a disposition of the kernel: the task is still there after
    # the signal, and its kill() answered 0.
    expect(
        "an ignored SIGTERM leaves the task alive",
        "PROGRAM: ignored SIGTERM, kill 0, still alive" in signals,
    )
    # SIGTERM by default ends the child, and the parent sees 128 + 15.
    expect(
        "SIGTERM ends the child with 128 + 15",
        re.search(r"PROGRAM: term child waited \d+, status 143", signals) is not None,
    )
    # The wait was cut by SIGINT, the handler ran, and the child was not
    # taken by the wait: EINTR is 4.
    expect(
        "SIGINT cuts a wait with EINTR and runs the handler",
        "PROGRAM: wait returned 4, handler runs 1" in signals,
    )
    expect(
        "kill of a pid that is not there is ESRCH",
        "PROGRAM: kill ghost returned 3" in signals,
    )
    expect(
        "a signal outside the table is EINVAL",
        "PROGRAM: kill signal 99 returned 22" in signals,
    )
    # The child of this line never makes a call: the handler can only arrive
    # from the tick of the timer, with the frame of the interrupt.
    expect(
        "the tick delivers a handler to a program that does not call",
        "PROGRAM: tick handler, waited" in signals,
    )
    expect(
        "the signals program returns its status",
        "the program exited with 0" in signals and "the program has finished" in signals,
    )

    # --- the edge of the system call: a bad pointer is EFAULT, not a fault
    # The numbers below are the answer of the call: 14 is EFAULT (POSIX 14,
    # the minus of the kernel dropped when the program prints it). The three
    # addresses are a window that is not there, the last bytes of the window
    # with a request that crosses its end, and the guard page under the stack.
    edge = answer("exec /bin/badptr")
    expect("the edge refuses a far pointer", "PROGRAM: write far 14" in edge)
    expect("the edge refuses a range past the window", "PROGRAM: write edge 14" in edge)
    expect("the edge refuses the guard page", "PROGRAM: write guard 14" in edge)
    expect("the edge refuses the buffer of stat", "PROGRAM: stat far 14" in edge)
    expect("the edge refuses the buffer of read", "PROGRAM: read far 14" in edge)
    # The same call with a valid pointer still works: the answer is about the
    # address, not about a call that refuses everything.
    expect(
        "the edge keeps the valid pointer",
        "PROGRAM: the good pointer still writes" in edge,
    )
    expect("the edge program returns its status", "the program exited with 0" in edge)
    expect("the shell sees the edge program finish", "the program has finished" in edge)

    # --- the pages of a program: NX on the data, W^X for the text
    # Every fault is vector 14 and the error code is what says which access
    # broke which rule of the two: 0x15 is a fetch from an NX page, 0x7 a store
    # into the read-only text. The child that fetched the data never printed
    # the line after the fetch, and the child that wrote the text never printed
    # the line after the store: a machine without the protections would print
    # both and the parent would see a status of 1 instead of the 255 of a
    # fault.
    nx = answer("exec /bin/nx")
    expect("the nx program starts", "PROGRAM: nx starts" in nx)
    expect(
        "the child tries to execute the data page",
        "PROGRAM: the child executes the data page" in nx,
    )
    expect(
        "the data of a program is not executable",
        "the program faulted: vector 14 at 0x" in nx
        and "error 0x0000000000000015" in nx,
        "there is no page fault with the instruction-fetch bit",
    )
    expect(
        "the data page never ran",
        "PROGRAM: the data page executed" not in nx,
    )
    expect(
        "the child tries to write the text page",
        "PROGRAM: the child writes the text page" in nx,
    )
    expect(
        "the text of a program is not writable",
        "error 0x0000000000000007" in nx,
        "there is no page fault of a write to the text",
    )
    expect(
        "the text page took no write",
        "PROGRAM: the text page took a write" not in nx,
    )
    expect(
        "the parent waits for both faults",
        "PROGRAM: execute data, status 255" in nx
        and "PROGRAM: write text, status 255" in nx,
    )
    # The third child hands its text to read(): the kernel's copy edge makes a
    # private page of data for the store, the call answers, and the next fetch
    # from that page is the fault of the child. The parent is alive, and the
    # original frame - the code it shares with the child - was not touched.
    expect(
        "a call writes the text in a private copy",
        "PROGRAM: the child reads into the text page" in nx
        and "PROGRAM: kernel write text, status 255" in nx,
    )
    expect(
        "the kernel write does not reach the code of the parent",
        "PROGRAM: the kernel wrote the text page" not in nx,
    )
    expect(
        "the nx program survives the three faults",
        "PROGRAM: the three faults left the machine alive" in nx
        and "the program exited with 0" in nx,
    )
    expect("the shell sees the nx program finish", "the program has finished" in nx)

    # --- the fault of a program is not the fault of the machine
    crash = answer("exec /bin/crash")
    expect("the faulting program starts", "PROGRAM: crash writes to address zero" in crash)
    # The page fault of ring 3 is reported and the program is taken down; the
    # address is the one the program wrote to, not the RIP of the instruction.
    expect(
        "the program fault is reported",
        "the program faulted: vector 14 at 0x0000000000000000" in crash,
    )
    expect(
        "the shell survives a fault of a program",
        re.search(r"MicroOS \d+\.\d+\.\d+", answer("uname", 1)) is not None,
    )

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image with /bin of the disk")
    parser.add_argument(
        "--open-build",
        default=os.path.join(ROOT, "build-x86-open"),
        help="the build with MICRO_OS_REQUIRE_SIGNATURE=OFF",
    )
    parser.add_argument("--timeout", type=float, default=30.0, help="the seconds the machine gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel, args.image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    # The commands of the shell change the volume (chmod, mkdir, rm), so the
    # run gets a copy of the image the build made: a test that failed in the
    # middle must not leave /bin/hello without its x bit for the next one.
    shutil.copyfile(args.image, RUN_IMAGE)

    try:
        output = run_machine(loader, kernel, RUN_IMAGE, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        if os.path.exists(RUN_IMAGE):
            os.remove(RUN_IMAGE)

    if args.verbose:
        print(output)

    checks = (
        linker_checks(KERNEL_ELF)
        + signature_checks(loader, kernel)
        + open_policy_checks(args.open_build)
        + check(output)
        + oversize_checks(loader, kernel, args.timeout)
    )
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the smoke test of the x86-64 port")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)
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
