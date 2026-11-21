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
]

# The prompt of the shell. It is written in one call, so the log of another
# task never splits it, and the answers can be separated by it.
PROMPT = "microos> "

# What the kernel has to say before the shell exists. The banner is the proof
# that the whole chain works: the loader, the switch to long mode, the
# console of COM1, the contract of the port and the boot of the kernel.
EXPECTED = [
    "board: qemu_x86_64",
    "port: x86_64, contract 1.8",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
    # The port reads the tables back at the boot: the text of the kernel is
    # read-only, every data page is NX, and no page is both (ADR 0007).
    "x86-64: the map is W^X",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
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


def run_machine(loader: str, kernel: str, image: str, timeout: float) -> str:
    """Boot the machine, drive the shell and return the transcript.

    The output is read while the machine runs, not after it stops: the
    commands have to arrive after the shell is ready, and the answer of a
    command is the proof the next command may go out.
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
        # The disk of the machine: a raw image on the primary IDE channel.
        # The programs of /bin are files of the MicroFS volume in it; the
        # boot writes nothing to the volume (ADR 0008).
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writeback",
    ]

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
            while sent < len(COMMANDS) and text.count(PROMPT) > sent:
                process.stdin.write((COMMANDS[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            # The prompt after the last command means its answer is done.
            if text.count(PROMPT) > len(COMMANDS):
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
    # All fifteen are objects of the volume the boot mounted, with the mode
    # the tool gave them; a missing one is a tool or a build that lost it.
    expect(
        "the fifteen programs are files of the disk",
        all(
            name in bin_listing
            for name in (
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
    expect("cat reports a file the formatter prints", "cat: /no/such/file: error 2" in cat)
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

    # The shell's own `ls` is a list of names and not a tree; `tree` is the
    # command that draws the levels below a path. `/etc` was made above, so
    # the two answers are of a volume the boot and the shell both wrote.
    root_listing = answer("ls /")
    expect(
        "ls / lists the names",
        all(name in root_listing for name in ("bin/", "etc/", "proc/", "dev/")),
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
    expect("wc counts the lines and the bytes", "3 34 /etc/setting" in wc)
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
    # No file is descriptor 0. A program inherits the table of the shell
    # (ADR 0017), but the prompt does not open the console on descriptor 0
    # yet, so the read answers EBADF; a redirect or a pipe fills it.
    cat = answer("exec /bin/cat")
    expect("cat reads descriptor 0 when no file is named", "cat: (stdin): error 9" in cat)
    expect("cat of a closed stdin is a failure", "the program exited with 1" in cat)
    wc = answer("exec /bin/wc -l -")
    expect("wc reads descriptor 0 for a dash", "wc: -: cannot read" in wc)
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
    expect("the tree is gone", "ls: /tmp: error 2" in gone)
    # pwd is the directory of the task, read through the call: the shell
    # starts at /, and the block after the typed line is that one character.
    pwd = answer("exec /bin/pwd")
    expect("pwd prints the directory of the task", re.search(r"(?m)^/\r?$", pwd) is not None)
    expect("pwd is a program of the shell", "the program exited with 0" in pwd)
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

    checks = linker_checks(KERNEL_ELF) + check(output)
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
