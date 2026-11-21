#!/usr/bin/env python3
"""Micro OS — the smoke test of the x86-64 port.

The kernel of the host build is a process, and its smoke test runs it. This
one boots the kernel on a real (emulated) processor: QEMU loads the loader
of the port, the loader turns the long mode on and copies the kernel, and
the kernel prints its banner on the serial port. There is no operating
system under it and no libc.

Then the shell has to answer: the commands are typed into the machine once
the console is up, and their answers are read back from the same transcript.
The prompt paces the typing on its own — the next command goes out when the
shell prints the prompt of the previous one — so a line is never split by
the small FIFO of the serial port of the emulated chip.

Between the boot and the shell the port of this machine brings up a filesystem
and runs a program out of it: a volume in its memory is mounted at /, the image
of /bin/hello is written to it and given the mode of a program, and the boot
asks for it with execve. The program is not a creature of the port: it prints
through the system call `write`, asks the kernel its pid and the time, sleeps
and returns a status, and every line of it below is a line the kernel produced
for it (ADR 0006). The transcript is the proof: a kernel that only printed a
banner could not have a number of a task in the middle of it.

    tools/x86_smoke.py [--timeout SECONDS] [--verbose]

The exit status is 0 when the kernel came up, the program ran out of a file and
the shell answered, and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import select
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
    "ls /bin",
    "chmod 0644 /bin/hello",
    "exec /bin/hello",
    "chmod 0755 /bin/hello",
    "chown 0 /bin/hello",
    "exec /bin/hello",
    "exec /bin/hello one two",
    # The three programs of the library (ADR 0016): a file through open/read/
    # write and the formatter, a line of arguments, and a directory through
    # readdir. The file they read is made here, because the disk of the test
    # is fresh at every boot.
    "mkdir /etc",
    "write /etc/setting hello",
    "exec /bin/cat /etc/setting",
    "exec /bin/cat /no/such/file",
    "exec /bin/echo one two",
    "exec /bin/ls /bin",
    # A program that starts another program with an environment of its own:
    # the third argument of execve copied from the memory of a program, not
    # the four words of the kernel that every call without one gets.
    "exec /bin/spawn",
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
    "port: x86_64, contract 1.7",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")


def run_machine(loader: str, kernel: str, timeout: float) -> str:
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

    # --- the program of the milestone
    # The line in the middle is not written by the kernel: it comes out of the
    # image that was written to and read back from /bin/hello. The lines after
    # it are answers of the kernel to its calls - a pid, a time and a status
    # that the program could not invent - so the transcript shows the door and
    # the way back, not only a string.
    expect("the filesystem takes the image", "x86-64: /bin/hello is written" in output)
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
    # The volume is not part of the kernel image: the file was written to it at
    # the boot, so a listing is the proof that the bytes are on a filesystem and
    # not a line the kernel printed about itself.
    expect("the program is a file in /bin", "hello" in answer("ls /bin"))
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
    parser.add_argument("--timeout", type=float, default=30.0, help="the seconds the machine gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    try:
        output = run_machine(loader, kernel, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    if args.verbose:
        print(output)

    checks = check(output)
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
