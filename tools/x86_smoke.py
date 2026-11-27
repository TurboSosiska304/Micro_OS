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

The timeout is a limit of the whole run of the command list and not of one
command: the default of two minutes is the measure of a machine that reads the
image of every program it runs (zstd is 437 KiB, lua 168 KiB).

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

from qemu_session import DISKS, drive_args, root_name

# The bus of the disk of this run, set by `--disk`: the image is one file and
# the two disks of the port are two devices (ADR 0037), so the lines of the
# transcript name the device of the run and nothing else changes.
DISK = "ide"

# The processor of the run, set by `--cpu`. Empty is the machine of every
# release: QEMU's own default, which has neither SMEP nor SMAP, and the
# supervisor bit of every kernel page is the boundary (ADR 0052). A run with
# `--cpu qemu64,+smep,+smap` boots the same image on the processor of a
# machine of 2014 and puts every copy of a program's memory through the
# window of the port; it is the other half of `make mmu-smoke`, over the whole
# suite and not only over the attacks.
CPU = ""

# The commands that are typed into the shell. The answers are read by their
# place in the list, the way the smoke test of the simulator does it. The last
# one looks at the tree the boot left behind: a program that ran out of a file
# is a program that is still in /bin afterwards.
COMMANDS = [
    "help",
    "kill 9999",
    # The console itself is the only shell (ADR 0033): a signal to the task
    # that runs the command leaves a machine that answers nothing until it is
    # reset, so the refusal is a sentence (pid 3 is the shell of the boot).
    "kill -TERM 3",
    "uname",
    "uptime",
    "date",
    "ulimit",
    "ls /bin",
    # The same directory with the mode of every entry: the programs are the
    # lines of a regular file, and a directory of /bin (`ports`) is not one.
    "ls -l /bin",
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
    # The test of the streams: ungetc, fread and a relative seek.
    "exec /bin/streams",
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
    # The pattern is one word whether or not it is quoted: the lexer of the
    # shell hands the text to the program as it was typed.
    "exec /bin/pcre2grep -i h.LLO /etc/setting",
    "exec /bin/pcre2grep -o -g 1 ^hostname=(x86_64)$ /etc/setting",
    "exec /bin/ls /etc/licenses",
    # The same commands with a file of no bytes: they read descriptor 0, which
    # is the terminal of the task while the shell opened nothing on it
    # (ADR 0051 §2). The gate gives them /dev/null so that the read ends
    # instead of waiting for a person, and the line stays the proof that the
    # input is descriptor 0 and not a usage sentence.
    "exec /bin/cat < /dev/null",
    "exec /bin/wc -l - < /dev/null",
    # The tree through the calls: a directory made, a directory refused by
    # rm without -r, the tree taken down with the flag, and a listing of a
    # directory that is no longer there.
    "mkdir /tmp",
    # The cut of a file through ftruncate: a file of ten bytes becomes four,
    # the bytes that are left are the first four, and a name that is not
    # there is the error of the call. The file lives until `rm -r /tmp`.
    "write /tmp/trunc 0123456789",
    "exec /bin/truncate 4 /tmp/trunc",
    "exec /bin/cat /tmp/trunc",
    "exec /bin/wc -c /tmp/trunc",
    "exec /bin/truncate 1 /no/such/file",
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
    # The third port of somebody else's program (zstd): the frame of the
    # fixture was written by the `zstd` of the host, and the guest unpacks it
    # with the library of the release. The reverse - a frame the guest wrote,
    # unpacked by the `zstd` of the host - is a boot of its own below
    # (`zstd_compat_checks`), because its answer is a file of bytes and not a
    # line of this transcript. Every `exec` of the command reads an image of
    # 437 KiB through the disk driver of the port, which is the slowest step
    # of this list (docs/debts.md): the eight lines below are the choices of
    # one of each thing, not five variations of a round trip.
    "exec /bin/zstd -d -c /etc/setting.zst",
    "exec /bin/zstd -19 -q -f /etc/setting -o /tmp/setting.zst",
    "exec /bin/zstd -d -c /tmp/setting.zst",
    "exec /bin/wc -c /tmp/setting.zst",
    # A frame with its tail cut is refused: the edge of the decoder.
    "exec /bin/zstd -t /etc/broken.zst",
    # The listing and the second name of the command (`zstdcat` is the same
    # image under a name, and the mode follows the name).
    "exec /bin/zstd -l /etc/setting.zst",
    "exec /bin/zstdcat /etc/setting.zst",
    # A stream through a pipe: the decompressor reads descriptor 0 of a stage
    # and writes descriptor 1 of it (`--stdout` is the long name of `-c`).
    "cat /etc/setting.zst | /bin/zstd -d --stdout",
    # A directory through `<dirent.h>` and the POSIX `stat` of the library of
    # a program: `-r` walks it and writes a frame beside every file.
    "mkdir /tmp/z",
    "echo alpha > /tmp/z/a",
    "echo beta > /tmp/z/b",
    "exec /bin/zstd -r -q -f /tmp/z",
    "exec /bin/ls /tmp/z",
    # `--rm` takes the source away when the frame is written: the copy is a
    # file of the volume and the original is not.
    "echo gamma > /tmp/rm.txt",
    "exec /bin/zstd --rm -q -f /tmp/rm.txt",
    "exec /bin/ls /tmp",
    "exec /bin/ls /etc/licenses",
    # The second port of a program of somebody else (Lua): the interpreter is
    # a file of /bin, its first script and its license are files of /etc, and
    # the number of a program with a point in it is the arithmetic of the
    # library of the guest. The image is 164 KiB and K_EXEC_MAX_IMAGE (8 MiB)
    # lets sys_execve read it in pieces of KSYS_MAX_IO; the run of the
    # library is the host test `tests/cases/test_lua.c`.
    "exec /bin/lua -e print(2^10)",
    "exec /bin/lua -e print(math.pi)",
    # The quotes of the shell are part of the line now (1.2.1): the Lua
    # expression is one word in single quotes, so the double quotes of the
    # format reach the interpreter as characters of the program.
    "exec /bin/lua -e 'print(string.format(\"%.3f\",1/3))'",
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
    # The third port of a program of somebody else (awk): the interpreter is
    # a file of /bin, its data and its script are files of /etc, and every
    # line below is a script the shell passed as words. The shell has no
    # quotes, so a program with a space in it is a word of its own - the
    # scripts of the machine are files, and the inline ones are written
    # without a space.
    "exec /bin/awk \"BEGIN{printf\\\"%.2f\\n\\\",1/3}\"",
    "exec /bin/awk -F: {print$1} /etc/columns",
    "exec /bin/awk -F: {s+=$3}END{print(s)} /etc/columns",
    "exec /bin/awk -v OFS=, -F: {print$1,$2} /etc/columns",
    "exec /bin/awk -f /etc/hello.awk /etc/columns",
    "exec /bin/awk \"{gsub(/o/,\\\"0\\\");print}\" /etc/setting",
    "exec /bin/awk \"BEGIN{while((getline<\\\"/etc/setting\\\")>0)n++;print(n)}\"",
    "exec /bin/awk \"BEGIN{printf\\\"%5.2f|%d|%s\\n\\\",3.14159,42,\\\"s\\\"}\"",
    "exec /bin/awk \"BEGIN{printf\\\"%*.*f|%i|%e\\n\\\",8,2,3.14159,42,1234.5}\"",
    "exec /bin/awk \"BEGIN{printf\\\"%c%c|%x|%o\\n\\\",65,66,255,8}\"",
    "exec /bin/awk -v x=42 BEGIN{print(x+1)}",
    "exec /bin/awk \"BEGIN{print(ENVIRON[\\\"PATH\\\"])}\"",
    "exec /bin/awk \"BEGIN{n=split(\\\"a:b:c\\\",p,\\\":\\\");print(n,p[2])}\"",
    "exec /bin/awk \"BEGIN{print(substr(\\\"hello\\\",2,3),index(\\\"hello\\\",\\\"ll\\\"),length(\\\"hello\\\"))}\"",
    "exec /bin/awk \"BEGIN{print(toupper(\\\"abc\\\"),tolower(\\\"DEF\\\"))}\"",
    "exec /bin/awk BEGIN{print(sqrt(2),exp(1),log(10),sin(0),cos(0),atan2(1,1))}",
    "exec /bin/awk \"BEGIN{printf\\\"%.3f\\n\\\",sqrt(2)}\"",
    "exec /bin/awk \"BEGIN{printf\\\"%f|%g|%.2e\\n\\\",1/3,1/3,1/3}\"",
    "exec /bin/awk \"BEGIN{s=sprintf(\\\"%.2f\\\",1/3);print(s)}\"",
    "exec /bin/awk BEGIN{print(int(3.9),int(-3.9),3%2,2^10,2**10)}",
    "exec /bin/awk \"BEGIN{s=\\\"hello\\\";print(match(s,/ll/),RSTART,RLENGTH)}\"",
    # The numeric string of a field: the third column of the data is 0, 1
    # and 1000, and a field compares as a number against the number 9.
    "exec /bin/awk -F: {print($3>9)} /etc/columns",
    "exec /bin/awk \"BEGIN{srand(1);r=rand();print(r>=0&&r<1)}\"",
    # The status of a script that ends with `exit N` comes back to the shell.
    "exec /bin/awk BEGIN{exit(3)}",
    # The two calls of a shell. `system` is the port layer of awk, which still
    # answers with its own sentence (the port of the release owns the name);
    # `"cmd" | getline` reaches `popen` of the library now (1.8) and runs the
    # program of the command, so the line is the one `echo` wrote.
    "exec /bin/awk \"BEGIN{print(system(\\\"true\\\"))}\"",
    "exec /bin/awk \"BEGIN{print(\\\"echo\\\"|getline)}\"",
    # A command that reads a file: `popen` of the library runs `/bin/cat`, the
    # stream is a pipe of this system, and the loop is the three lines of
    # /etc/setting. The interpreter is the release; the port of the command is
    # the library.
    "exec /bin/awk \"BEGIN{while((\\\"cat /etc/setting\\\"|getline line)>0)n++;print(n)}\"",
    "exec /bin/awk -f /etc/no-such.awk",
    # The same script with no file: the interpreter reads descriptor 0, which
    # at the prompt is the terminal of the task (ADR 0051 §2), so the gate
    # points it at a file and the read ends.
    "exec /bin/awk {print} < /etc/setting",
    "echo hi > /tmp/f",
    "cat /tmp/f",
    "cat < /tmp/f",
    "echo one | cat",
    "echo one | /bin/cat",
    "cat /etc/setting | wc -l",
    "ls /bin | wc -l",
    # The words of a line may be quoted (1.2.1): a word holds a space, an
    # operator inside the quotes is a character of the word, and a program
    # receives the text as it was typed instead of a word cut at the space.
    "echo 'a b' > /tmp/q",
    "cat /tmp/q",
    "grep 'a b' /tmp/q",
    "sed 's/a b/c d/' /tmp/q",
    "echo 'a  b'",
    'echo "a  b"',
    "echo 'a | b'",
    'echo "x > y"',
    # `help` is the list of what the machine has: the programs of PATH and
    # the two sides of a name that is a command and a file (`ls`).
    "help hello",
    "help ls",
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
    # The POSIX pair of a program (M1 of plan-1.5): a child that computes and
    # returns its status, a double fork, and an execve that replaces the child
    # itself instead of starting a second task.
    "exec /bin/procs",
    # The state of the floating point unit at a switch (K9): a parent and two
    # children compute in `double` at the same time, and each of them prints
    # the numbers of the registers it kept across many quanta.
    "exec /bin/fpu2",
    # A pipeline is not three stages any more. The line holds ten of them, and
    # the pipes are made one boundary at a time, so the descriptor table of
    # the shell holds two ends and not eighteen.
    "cat /etc/setting | cat | cat | cat | cat | cat | cat | cat | cat | wc -l",
    # The documented maximum of a chain (docs/wiki/shell.md): a stage that
    # runs a program is two tasks of the kernel - the stage and the program -
    # and the table of the machine has 32 slots, so fifteen stages of programs
    # fit and sixteen do not. Sixteen stages of commands of the shell fit,
    # because a stage of the shell is one task. The chain that does not fit is
    # refused before a pipe is made, with the numbers, so no part of it is
    # started and lost.
    "cat /etc/setting | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | "
    "cat | wc -l",
    "echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | "
    "echo | echo | echo sixteen",
    "cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat",
    # `xargs -P` starts its commands as children of its own and runs four of
    # them at once: each line is a child that was told to run /bin/echo, and a
    # child of a fork cannot be one without the POSIX execve.
    "echo one two three four five six | xargs -P 4 -n 1 echo done",
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
    # The same failure one page lower (ADR 0048): the recursion runs past the
    # bottom of the stack into the page the port leaves unmapped, the fault
    # takes the program down and the shell answers again.
    "exec /bin/crash stack",
    # And the stack of a task of the kernel: the command starts a task that
    # overflows its own 32 KiB stack, waits for it, and the answer is the
    # status 255 of a task that ended in the guard page.
    "guard",
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
    # The directory of the task is the kernel's (call 48): a program moves it,
    # a child it starts begins there, and the shell that ran the program keeps
    # its own - the program has a record of its own.
    "exec /bin/chdir",
    "pwd",
    # The heap of a program grows with `brk` (call 47): the window of the
    # desktop is 800 by 600 pixels, and the memory limit of the task is what
    # the new heap is charged to.
    "exec /bin/heap",
    "exec /bin/limit",
    # The clock of the machine (call 49): a file written now carries the date
    # of the person and not the year of the records of the image, which the
    # tool stamped with SOURCE_DATE_EPOCH = 0.
    "write /tmp/dated.txt x",
    "exec /bin/ls -l /tmp/dated.txt",
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
    # The type of a file, with libmagic (third_party/file): a program of the
    # image is a MOX1 record of our database, the fixture is a gzip, the
    # script is Lua, a directory is a directory and a name that is not there
    # is the sentence of the call and its status. The database is the compact
    # subset of /etc/magic.mgc, so the recognitions below are the ones the
    # port promised and not the ten megabytes of the release.
    "exec /bin/file /bin/file",
    "exec /bin/file -i /bin/file",
    "exec /bin/file /etc/greeting.gz",
    "exec /bin/file -z /etc/greeting.gz",
    # The second compression of the image: the frame of the host written into
    # /etc/setting.zst, unpacked by the libzstd the command is linked against.
    "exec /bin/file -z /etc/setting.zst",
    "exec /bin/file /etc/hello.lua",
    "exec /bin/file -i /etc/hello.lua",
    "exec /bin/file -b /etc/setting",
    "exec /bin/file /etc",
    "exec /bin/file /etc/no-such-file",
    "echo /etc/setting > /tmp/filelist",
    "exec /bin/file -f /tmp/filelist",
    # The bytes of descriptor 0 through a pipe, and the flag of a link that
    # the VFS does not have (accepted, and the file is read).
    "cat /etc/setting | /bin/file -",
    "exec /bin/file -L /etc/setting",
    # SQLite (third_party/sqlite): the database library of the public domain
    # and the shell of the release. The scripts are files of /etc because the
    # shell has no quotes; the first run creates the table, the second one
    # reads it in another program, and `.read` and `-cmd` are the two shapes
    # of the shell that open a file while the database is open.
    "exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-setup.sql",
    "exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-read.sql",
    "exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-nested.sql",
    "exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-schema.sql",
    "exec /bin/sqlite3 -cmd .tables /tmp/sqlite.db < /etc/sql-read.sql",
    # The timer of the shell is the one line of the guest that prints a real
    # number through the formatter of the library (`%f`, six digits).
    "exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-timer.sql",
    # `-batch` is the shell of the release with no prompt and no editor: the
    # script of a redirect is the same one and the answers are the same.
    "exec /bin/sqlite3 -batch /tmp/sqlite.db < /etc/sql-read.sql",
    # The first lane of 1.6. `poll` waits for a descriptor in the scheduler:
    # two pipes, the byte a child writes while the task waits, a deadline and
    # a sleep of no descriptor. `mmap` reads a file through a window of
    # mappings, writes a private page that the file does not take and a shared
    # one that it does, and moves the heap until it meets the stack. `big` is
    # a program of 4 MiB that measures the memory it costs: the image is
    # mapped from the file, so the heap of the kernel does not grow with it.
    "cat /proc/meminfo",
    "exec /bin/poll",
    "exec /bin/mmap",
    "head -n 1 /mm.txt",
    "wc -c /mm.txt",
    "exec /bin/big",
    "cat /proc/meminfo",
    "rm /mm.txt",
    # The sentences of the files. `mkdir` takes every word of the line, a name
    # that no volume of this system holds is `file name too long` (MicroFS
    # keeps 32 bytes) and not the code of the volume, and `df` names the unit
    # of its blocks - a kilobyte here and not "0 KB" somewhere.
    "mkdir /tmp/m1 /tmp/m2",
    "stat /tmp/m2",
    "mkdir /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "write /tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa x",
    "df /",
]

# A directory wider than the table of 64 entries the shell used to keep: the
# files are written one command at a time, and the listing and the walk have
# to name and count all of them. `rm -r` puts the volume back for the checks
# that follow (the run works on a copy of the image, but a clean tree is
# easier to read in a failure).
MANY_FILES = 70
COMMANDS += ["ls /bin/ports"]
COMMANDS += ["mkdir /many"]
COMMANDS += [f"write /many/f{index:02d} x" for index in range(MANY_FILES)]
COMMANDS += ["ls /many", "tree /many", "rm -r /many"]

# The prompt of the shell. It is written in one call, so the log of another
# task never splits it, and the answers can be separated by it.
PROMPT = "microos> "


def prompt_count(text: str) -> int:
    """The shell prompts in `text`, not prompt text inside a logged line."""
    return len(re.findall(r"(?m)^microos> ", text))

# What the kernel has to say before the shell exists. The banner is the proof
# that the whole chain works: the loader, the switch to long mode, the
# console of COM1, the contract of the port and the boot of the kernel.
def board_line() -> str:
    """The line of the board of the port, read from the source of the machine.

    The x86-64 port is not QEMU's board - the name says what the machine of the
    port is - and a rename there is one edit of `kmain.c` and not one edit of
    every smoke of the port (`board: x86_64` since the wire of 2.0).
    """
    try:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(root, "ports/port_x86_64/src/kmain.c")
        with open(path, encoding="utf-8") as handle:
            found = re.search(r'\.name\s*=\s*"([^"]+)"', handle.read())
        if found is not None:
            return "board: " + found.group(1)
    except OSError:
        pass
    return "board: x86_64"


EXPECTED = [
    # The loader checked the trailer of the kernel before it copied a byte of
    # it (ADR 0020); the line is the proof that the check ran and passed.
    "Micro OS: image signature ok (sha-256, ed25519)",
    board_line(),
    "port: x86_64, contract 1.15",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
    # The port reads the tables back at the boot: the text of the kernel is
    # read-only, every data page is NX, and no page is both (ADR 0007).
    "x86-64: the map is W^X",
    "gpu: no display controller of a known vendor, the framebuffer is the firmware's",
    "kmod: gpu skip",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")

# The number of processors of the machine of the smoke test (ADR 0012). It is
# a constant of the test and not a setting: the point of the line is that the
# firmware describes two and the kernel knows how many it has started.
MACHINE_SMP = "2"
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")

# The names an image of the release promises: the one list of the tree
# (`tests/ports/promised.txt`), read by the gate of the machine here and by
# `tools/ports_smoke.py` for the half that the SDK builds. A name here that the
# image does not carry is the defect the owner's laptop found in 1.9: `kilo`
# in the notes and not in the image, `date` a command of the board only.
PROMISED_FILE = os.path.join(ROOT, "tests", "ports", "promised.txt")


def read_promised(path: str = PROMISED_FILE) -> list:
    """The lines of the file of promises, as (name, where) pairs.

    A line is `name where`, a `#` begins a comment, and `where` is `/bin`,
    `/bin/ports` or `shell`. A line that is not one of those is an error of
    the file: a promise the gate cannot read is a promise nobody keeps.
    """
    promises = []

    if not os.path.isfile(path):
        raise RuntimeError(f"missing {path}: the list of the names of a release")

    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue

            fields = line.split()
            if len(fields) != 2 or fields[1] not in ("/bin", "/bin/ports", "shell"):
                raise RuntimeError(f"{path}:{number}: {line!r} is not `name /bin`, `name /bin/ports` or `name shell`")

            promises.append((fields[0], fields[1]))

    if not promises:
        raise RuntimeError(f"{path} is empty")

    return promises


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
        board_line() in transcript and "the kernel is up" in transcript,
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
        board_line() in transcript and "the kernel is up" in transcript,
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
        "the kernel is up" not in refused and board_line() not in refused,
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


def machine_argv(loader: str, kernel: str, image: str, disk: str = None) -> list:
    """The command line of the machine of the smoke tests.

    One machine for the run of the commands, the script of the console of the
    accounts and the oversized image: a second copy of these arguments would
    be a second machine, and the checks would read the boot of another one.

    The bus of the disk is the one of the run (the global `DISK`, from
    `--disk`) unless the caller names another: the machines of a test of this
    tool all boot the same medium.
    """
    disk = DISK if disk is None else disk

    return [
        "qemu-system-x86_64",
        *(["-cpu", CPU] if CPU else []),
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
        # The disk of the machine: a raw image on the primary IDE channel of
        # ADR 0008, or on the virtio device of ADR 0037 (`--disk virtio`).
        # The programs of /bin are files of the volume in it; the boot writes
        # nothing to the volume.
        *drive_args(image, disk),
    ]


def run_machine(loader: str, kernel: str, image: str, timeout: float, commands: list = None,
                disk: str = None) -> str:
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
    command = machine_argv(loader, kernel, image, disk)

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
            # A prompt saved in /etc/run.log is file content and must not send
            # the next command early.
            prompts = prompt_count(text)
            while sent < len(lines) and prompts > sent:
                process.stdin.write((lines[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            # The prompt after the last command means its answer is done.
            if prompts > len(lines):
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
    """A 32 MB disk with one file of K_EXEC_MAX_IMAGE + 1 zero bytes.

    The volume is written once and kept: it carries the programs of the build
    too, but this boot runs only the big file and the shell, so a rebuild of
    the kernel does not make it stale, and the 8 MiB of blocks are the slowest
    part of the smoke test. The volume is 32 MB and not 16: the image of the
    programs carries a program of 4 MiB (/bin/big, the measurement of the
    memory of a run), and 8 MiB of file plus 4 MiB of program do not fit a
    volume of 16.
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
            "32",
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


# The script of the image that writes the bytes of a file as hexadecimal.
HEX_SCRIPT = "/etc/hex.lua"


def host_zstd_decompress(frame: bytes) -> bytes:
    """The frame of the guest, unpacked by the zstd of the host.

    The command of the machine is the other half of the test of the format: a
    program of Micro OS wrote the bytes, and the zstd of the machine that runs
    the test has to read them. A host without the command falls back on the
    module of Python 3.14, which is the same format and the same answer.
    """
    tool = shutil.which("zstd")
    if tool is not None:
        result = subprocess.run(
            [tool, "-d", "-c"], input=frame, capture_output=True, check=False
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())
        return result.stdout

    from compression import zstd

    return zstd.decompress(frame)


def zstd_compat_checks(loader: str, kernel: str, timeout: float) -> list:
    """The frame of the guest, unpacked by the zstd of the host.

    The fixture of `/etc/setting.zst` is the other direction - a frame of the
    host that the guest reads - and it is a line of the main transcript. This
    boot has an answer of bytes, and the console of the port is a screen: it
    drops what is not a letter and writes a CR before every LF. The bytes
    therefore leave the machine as the text of a Lua program that prints them
    as hexadecimal pairs (`/etc/hex.lua`), and the test packs them back into a
    file and asks the `zstd` of the host for the script it compressed.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    image = os.path.join(BUILD, "zstd-compat.img")
    shutil.copyfile(DEFAULT_IMAGE, image)
    commands = [
        "mkdir /tmp",
        # The source is the script of the first Lua program of the machine:
        # 424 bytes of text, so a frame with a header, a block and a footer.
        "exec /bin/zstd -q -f -3 /etc/hello.lua -o /tmp/frame.zst",
        f"exec /bin/lua {HEX_SCRIPT} /tmp/frame.zst",
        "exec /bin/wc -c /tmp/frame.zst",
        "uname",
    ]

    try:
        output = run_machine(loader, kernel, image, timeout, commands)
    except FileNotFoundError:
        expect("qemu-system-x86_64 is installed", False)
        return checks
    finally:
        if os.path.exists(image):
            os.remove(image)

    blocks = output.split(PROMPT)[1:]
    expect(
        "the machine of the compatibility test boots",
        len(blocks) > len(commands),
        output[-300:],
    )

    hex_text = ""
    if len(blocks) > 2:
        for line in blocks[2].splitlines():
            line = line.strip()
            if len(line) >= 16 and len(line) % 2 == 0 and re.fullmatch(r"[0-9a-f]+", line):
                hex_text = line
                break

    expect(
        "the guest prints the bytes of its frame",
        len(hex_text) >= 32,
        blocks[2][:200] if len(blocks) > 2 else "",
    )

    source = os.path.join(BUILD, "hello.lua")
    try:
        with open(source, "rb") as file:
            expected = file.read()
    except OSError as error:
        expect("the script of the image is readable", False, str(error))
        return checks

    if hex_text:
        frame = bytes.fromhex(hex_text)
        expect("the frame of the guest starts with the magic of zstd", frame[:4] == b"\x28\xb5\x2f\xfd")
        try:
            unpacked = host_zstd_decompress(frame)
        except (OSError, RuntimeError, ValueError) as error:
            expect("the zstd of the host unpacks the frame of the guest", False, str(error))
            return checks
        expect(
            "the zstd of the host unpacks the frame of the guest",
            unpacked == expected,
            f"{len(unpacked)} bytes of {len(expected)}",
        )
        expect(
            "the bytes of the frame are the script of the image",
            b"the first script of the machine" in unpacked,
            unpacked[:120],
        )
    else:
        expect("the zstd of the host unpacks the frame of the guest", False, "no hex line")

    # The count of the frame and the frame agree: a hex line that lost a byte
    # would still pass the two checks above only if the host refused it.
    if len(blocks) > 3 and hex_text:
        match = re.search(r"(\d+) /tmp/frame.zst", blocks[3])
        expect(
            "the hex line is the whole frame",
            match is not None and int(match.group(1)) == len(hex_text) // 2,
            f"{match.group(1) if match else '?'} byte(s) in the file",
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
        f"x86-64: the root volume is {root_name(DISK)}" in output,
        "the machine booted without the image of the disk",
    )
    expect(
        "the boot finds the programs on the disk",
        f"x86-64: the programs come from {root_name(DISK)}:/bin" in output,
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
        "a kernel without KERNEL_SMP says the core 0 runs alone",
        "x86-64: this kernel is built without KERNEL_SMP, the core 0 runs alone" in output
        if "reported by the firmware, 1 in use" in output
        else True,
        "the kernel uses one core and does not say why",
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

    # The answers, separated by a prompt at the start of a line. The echo of
    # the typed line is part of the block; the checks below look for the answer
    # itself. A logged console line may contain the text of a prompt after
    # "/etc/run.log:", and that is file content, not the shell asking for more.
    blocks = re.split(r"(?m)^microos> ", output)[1:]

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
    # The list of the shell is not the list of the machine: the programs of
    # /bin are named below the commands, because `awk` or `pkg` is a file and
    # nobody registered it.
    expect(
        "help names the programs of PATH",
        "programs in PATH:" in answer("help") and "hello" in answer("help"),
        repr(answer("help")[-200:]),
    )
    expect(
        "help names a program of the disk",
        "hello is a program at /bin/hello (type hello --help)" in answer("help hello"),
        repr(answer("help hello")),
    )
    # The list of the programs is the machine and not a table of the shell, so
    # the list and the directory have to agree: the check reads `/bin` with the
    # shell and asks `help` for every name of it. A program that the image
    # gained and `help` does not name is a machine with a secret, which is what
    # the list of twenty-four names and a `...` was (`docs/debts.md`, K19).
    # The names of the listing are the lines without a space and without a
    # colon: the echo of the line and the lines of the log of the kernel are
    # not names of a program.
    programs = []
    for line in answer("ls -l /bin").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith("-") and len(fields[0]) == 10:
            name = fields[-1].split("/")[-1].strip()
            if name and ":" not in name and name not in programs:
                programs.append(name)
    listing = answer("help")
    absent = [name for name in programs if name not in listing]
    expect(
        "help names every program of /bin",
        bool(programs) and not absent,
        f"{len(programs)} program(s), not in help: {absent[:6]}",
    )
    # The promises of the notes and of the docs, asked of the machine. The list
    # is one file and not a second table here: a name of it is a line of the
    # directory that the list names, and a command of the shell is a line of
    # the table of `help` (a name of PATH is in the list of the programs).
    def lines_of(text: str) -> list:
        return [
            line.strip()
            for line in text.splitlines()
            if line.strip() and " " not in line.strip() and ":" not in line
        ]

    ports_programs = lines_of(answer("ls /bin/ports"))
    bin_programs = lines_of(answer("ls /bin"))
    help_text = answer("help")
    # The table of the commands ends where the list of the programs of PATH
    # begins: a line of the table is a name and a summary, and the programs
    # are named packed in the lines below the heading.
    table, _, programs_text = help_text.partition("programs in PATH:")
    help_commands = {
        line.split()[0]
        for line in table.splitlines()
        if line.strip() and line.split()[0] != "command"
    }

    missing_ports = []
    missing_bin = []
    missing_commands = []
    unnamed = []
    for name, where in read_promised():
        if where == "/bin/ports":
            if name not in ports_programs:
                missing_ports.append(name)
        elif where == "/bin":
            if name not in bin_programs:
                missing_bin.append(name)
        else:
            if name not in help_commands:
                missing_commands.append(name)

        # Every file of PATH is a name of the list of the programs of `help`,
        # which is how a person learns it is there at all.
        if where != "shell" and re.search(
            rf"(?<![A-Za-z0-9._-]){re.escape(name)}(?![A-Za-z0-9._-])", programs_text
        ) is None:
            unnamed.append(name)

    expect(
        "the programs of the SDK are in /bin/ports of the image",
        bool(ports_programs) and not missing_ports,
        f"{len(ports_programs)} program(s) of the SDK, missing: {missing_ports[:8]}",
    )
    expect(
        "every program the notes promise is a file of /bin",
        not missing_bin,
        f"missing from /bin: {missing_bin[:8]}",
    )
    expect(
        "every command the notes promise is a command of the shell",
        not missing_commands,
        f"missing: {missing_commands[:8]}",
    )
    expect(
        "help names every promised program of PATH",
        not unnamed,
        f"not in the list of help: {unnamed[:8]}",
    )

    help_ls = answer("help ls")
    expect(
        "help names the two sides of a name",
        "ls is a program: /bin/ls" in help_ls
        and "the builtin:" in help_ls
        and "usage: ls" in help_ls,
        repr(help_ls),
    )
    # The shell command of the signal: a pid no task has is a sentence, and
    # the shell of the machine is alive after it.
    expect(
        "kill of a pid that is not there is reported",
        "kill: 9999: no such process" in answer("kill 9999"),
    )
    # The console is the only shell: the signal to it is refused, and the
    # machine is still there for the next line of the list (which it proves by
    # answering it).
    console = answer("kill -TERM 3")
    expect(
        "kill of the console itself is refused with a sentence",
        "this is the console itself" in console and "exited with code" not in console,
        console.strip(),
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
    # The wall clock of the machine: the CMOS of QEMU read by the port at the
    # boot, printed as UTC. It was a command of the board's driver only, so
    # the laptop of the owner answered `command not found` for it, and when
    # the command came the port's read of the year was one pass of BCD too
    # many - 2026 printed as 2020 - so the check compares the day with the
    # host and not only the shape of the line.
    moment = answer("date")
    expect(
        "date prints the moment of the machine",
        re.search(r"(?m)^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\r?$", moment) is not None,
        repr(moment),
    )
    # The guest prints UTC (the RTC of the machine, not the zone of the host),
    # so between midnight and the offset of the host the two days differ and
    # the check compared a correct clock with the wrong day.
    day_utc = time.strftime("%Y-%m-%d", time.gmtime())
    expect(
        "date names the day of the machine and not another year",
        day_utc in moment,
        f"{repr(moment)}, the host is {day_utc} in UTC",
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

    # --- the sentences of the files and the numbers of the volume
    # `mkdir` takes one directory per word, as the program of /bin does: a
    # second word used to be dropped in silence.
    expect(
        "mkdir makes a directory for every word of the line",
        "type: directory" in answer("stat /tmp/m2"),
        answer("stat /tmp/m2"),
    )
    # A name longer than MicroFS keeps (32 bytes) is the limit named as a
    # limit, and not the code the volume uses for it.
    long_mkdir = answer("mkdir /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    expect(
        "a name longer than the volume holds is `file name too long`",
        "file name too long" in long_mkdir and "error" not in long_mkdir,
        long_mkdir.strip(),
    )
    long_write = answer("write /tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa x")
    expect(
        "a file name too long is `file name too long` for write",
        "file name too long" in long_write,
        long_write.strip(),
    )
    # The block of the volume is its own unit: a kilobyte on MicroFS, and the
    # line says which one (a FAT32 volume says "512 bytes each").
    root_df = answer("df /")
    expect(
        "df names the unit of its blocks",
        re.search(r"\(1 KB each\)", root_df) is not None and "0 KB each" not in root_df,
        root_df.strip(),
    )
    expect(
        "df counts the inodes of a volume that keeps them",
        re.search(r"inodes: \d+ of \d+ free", root_df) is not None,
        root_df.strip(),
    )

    # --- the tools of sbase (third_party/sbase) are the commands of /bin
    #
    # The shell looks in PATH before its own table for these ten names, so a
    # bare `ls` is the program of the disk (the kernel says `task: created
    # 'program'`) and not the command of the shell. The checks below read the
    # answer of the program and the escape of the colour.
    listing = answer("ls -la /etc")
    # The moment of a record of the image is SOURCE_DATE_EPOCH, which is zero:
    # the listing shows the year of a record of 1970 and not the date of the
    # machine. A file written by the machine today carries its date, and the
    # check of that one is below.
    expect(
        "ls -la prints the mode, the owner and the group",
        re.search(r"-rw-r--r--\s+1 root\s+root\s+34 Jan 01\s+1970 setting", listing) is not None,
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
    # The lines of the answer after the echo of the command. A missing block is
    # an empty answer and not an exception: a check that dies with a ValueError
    # tells the reader nothing about the machine (it happened once under load).
    order = [line.strip() for line in ordered.splitlines()[1:] if line.strip()]
    order = [line for line in order if not line.startswith(("task:", "x86-64:"))]
    expect(
        "sort orders the lines",
        order == ["console=vga", "hello", "hostname=x86_64"],
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
    # --- SQLite (third_party/sqlite)
    #
    # The first run creates the database and the table: three rows of the
    # three types are inserted, one in a transaction that rolls back, and the
    # library checks the file it wrote. The line `2|3.75` of the second run is
    # the count and the sum of the two rows that survived, read by a second
    # process from the same file of MicroFS.
    created = answer("exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-setup.sql")
    expect(
        "sqlite3 creates a database and its table",
        "1|one|1.5" in created and "2|two|2.25" in created,
        created,
    )
    expect(
        "the transaction that rolls back leaves no row",
        "roll" not in created and re.search(r"(?m)^2\r?$", created) is not None,
        created,
    )
    expect(
        "the integrity check of the new database is ok",
        re.search(r"(?m)^ok\r?$", created) is not None,
        created,
    )
    reread = answer("exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-read.sql")
    expect(
        "a second run reads the count and the sum of the file",
        "2|3.75" in reread,
        reread,
    )
    expect(
        "the REAL and the BLOB keep their type",
        "real|blob" in reread,
        reread,
    )
    expect(
        "the file of the second run passes the integrity check",
        re.search(r"(?m)^ok\r?$", reread) is not None,
        reread,
    )
    nested = answer("exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-nested.sql")
    expect(
        "the .read of the shell opens a script while the database is open",
        "2|3.75" in nested and "ok" in nested,
        nested,
    )
    schema = answer("exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-schema.sql")
    expect(
        "the shell prints the table and its schema",
        "items" in schema and "CREATE TABLE items" in schema,
        schema,
    )
    expect(
        "the mode and the row of the second row",
        "two" in schema,
        schema,
    )
    listed = answer("exec /bin/sqlite3 -cmd .tables /tmp/sqlite.db < /etc/sql-read.sql")
    expect(
        "the -cmd of the shell runs before the script",
        "items" in listed and "2|3.75" in listed,
        listed,
    )
    timed = answer("exec /bin/sqlite3 /tmp/sqlite.db < /etc/sql-timer.sql")
    expect(
        "the timer prints a real number through the formatter of the library",
        re.search(r"Run Time: real \d+\.\d{6} user", timed) is not None,
        timed,
    )
    batch = answer("exec /bin/sqlite3 -batch /tmp/sqlite.db < /etc/sql-read.sql")
    expect(
        "the shell runs a script in batch mode",
        "2|3.75" in batch and "ok" in batch,
        batch,
    )
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

    # --- the directory of the task (call 48, the lane of 1.8)
    #
    # `chdir` is a call of the kernel now: the programme moves the directory
    # of its own task, a file is refused with ENOTDIR, and the child of its
    # fork - a copy of the record - begins in /etc and prints it through
    # `pwd`, an image of another program. The shell that ran the programme is
    # still in /, because a program has a record of its own.
    moved = answer("exec /bin/chdir")
    expect(
        "a program begins in the directory of its task",
        re.search(r"(?m)^chdir: the program begins in /\r?$", moved) is not None,
        moved[:400],
    )
    expect("a program moves its directory", "chdir: cd /etc says /etc" in moved, moved)
    expect(
        "a file is not a directory",
        "chdir: a file is not a directory: 20" in moved and "chdir: after the refusal /etc" in moved,
        moved,
    )
    expect(
        "the child of a fork begins in the new directory",
        re.search(r"(?m)^/etc\r?$", moved) is not None
        and "chdir: the child ended with 0" in moved,
        moved,
    )
    expect(
        "the move of the child does not move the parent",
        "chdir: the parent is still in /etc" in moved,
        moved,
    )
    expect(
        "the chdir program ends with its status",
        "chdir: done" in moved and "the program exited with 0" in moved,
        moved,
    )
    expect(
        "a program does not move the directory of the shell",
        re.search(r"(?m)^/\r?$", answer("pwd", 4)) is not None,
        answer("pwd", 4),
    )

    # --- the heap of a program and the limit of its task (calls 47 and 33)
    #
    # The window of the desktop is 1 920 000 bytes, more than the arena of the
    # bss: the block comes from the machine through `brk`. A programme that
    # lowers its own memory limit cannot have that block any more, and a
    # smaller one still fits - which is the limit and not the heap.
    grown = answer("exec /bin/heap")
    expect("a window of 800 by 600 is a block of the heap", "heap: a window is 1920000 bytes" in grown, grown)
    expect(
        "the bytes of the tall window are the bytes of the program",
        "heap: the corners of the window say 102" in grown,
        grown,
    )
    expect(
        "a block that was freed is a block of the heap again",
        "heap: the second window ends with 68" in grown,
        grown,
    )
    expect(
        "the heap program ends with its status",
        "heap: done" in grown and "the program exited with 0" in grown,
        grown,
    )
    limited = answer("exec /bin/limit")
    expect(
        "a program reads the memory limit of its task",
        # The ceiling is a fact of the machine (ADR 0057): the window of the
        # programs on x86-64 and not the 2 MiB of a machine without one.
        re.search(r"limit: the memory limit is (\d+) bytes", limited) is not None
        and int(re.search(r"limit: the memory limit is (\d+) bytes", limited).group(1))
        >= 64 * 1024 * 1024,
        limited,
    )
    expect(
        "a program lowers the limit of its task",
        "limit: lowered to 1048576 bytes" in limited,
        limited,
    )
    expect("a raise of a limit is refused", "limit: a raise says 1" in limited, limited)
    expect(
        "the limit is what refuses the window",
        "limit: a window of 1920000 bytes is refused with 12" in limited,
        limited,
    )
    expect(
        "a block under the limit still fits",
        "limit: a block of 524288 bytes is taken and ends with 90" in limited,
        limited,
    )
    expect(
        "the limit program ends with its status",
        "limit: done" in limited and "the program exited with 0" in limited,
        limited,
    )

    # --- the clock of the machine (call 49)
    #
    # The machine reads the date from its own source at the boot (the CMOS of
    # this port, the process of the host, the chip of the board), so a record
    # written now carries the date of the person. The records of the image are
    # of 1970 - the tool stamped them with SOURCE_DATE_EPOCH - so the two
    # kinds of moment are told apart in one listing of one directory.
    dated = answer("exec /bin/ls -l /tmp/dated.txt")
    now = time.gmtime()
    stamp = time.strftime("%b %d", now)
    expect(
        "a file written now carries the date of the machine",
        stamp in dated and "1970" not in dated,
        f"{dated!r}, the host is {stamp}",
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

    # --- the type of a file (third_party/file, libmagic)
    #
    # The first check is the record of our own format: the image of a program
    # is MOX1 and no Magdir file of the release knows it. The others are the
    # families of the compact database: a compressed stream, a script of a
    # language, a directory and a name that is not there.
    image_type = answer("exec /bin/file /bin/file")
    expect(
        "file names a program of the image as a MOX1 executable",
        "Micro OS MOX1 executable" in image_type and "version 2" in image_type,
        image_type,
    )
    mime = answer("exec /bin/file -i /bin/file")
    expect(
        "file -i prints the MIME type of a MOX1 image",
        "application/x-microos-mox" in mime,
        mime,
    )
    packed = answer("exec /bin/file /etc/greeting.gz")
    expect("file names a gzip stream", "gzip compressed data" in packed, packed)
    inside = answer("exec /bin/file -z /etc/greeting.gz")
    expect(
        "file -z looks inside the gzip",
        "ASCII text" in inside and "gzip compressed data" in inside,
        inside,
    )
    # The same flag on a frame of zstd: the decompressor of the image reads the
    # content of /etc/setting.zst, which is the setting of the machine and not
    # the bytes a description of the frame would name.
    inside_zstd = answer("exec /bin/file -z /etc/setting.zst")
    expect(
        "file -z looks inside a zstd frame",
        "ASCII text" in inside_zstd and "Zstandard compressed data" in inside_zstd,
        inside_zstd,
    )
    lua = answer("exec /bin/file /etc/hello.lua")
    expect("file names a Lua script", "Lua script" in lua, lua)
    lua_mime = answer("exec /bin/file -i /etc/hello.lua")
    expect("file -i names the Lua MIME type", "text/x-lua" in lua_mime, lua_mime)
    brief = answer("exec /bin/file -b /etc/setting")
    expect(
        "file -b prints the description and not the name",
        "ASCII text" in brief and "/etc/setting:" not in brief,
        brief,
    )
    directory = answer("exec /bin/file /etc")
    expect("file names a directory", "directory" in directory, directory)
    missing = answer("exec /bin/file /etc/no-such-file")
    expect(
        "file reports a name that is not there",
        "cannot open" in missing and "No such file or directory" in missing,
        missing,
    )
    listed = answer("exec /bin/file -f /tmp/filelist")
    expect(
        "file -f reads the names of a list",
        "/etc/setting: ASCII text" in listed,
        listed,
    )
    piped_bytes = answer("cat /etc/setting | /bin/file -")
    expect(
        "file reads the bytes of descriptor 0",
        "-: ASCII text" in piped_bytes,
        piped_bytes,
    )
    followed = answer("exec /bin/file -L /etc/setting")
    expect(
        "file -L accepts the flag of a link",
        "ASCII text" in followed,
        followed,
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
                "procs",
                "fpu2",
                "nx",
                "cat",
                "echo",
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
                "zstd",
                "zstdcat",
                "file",
                "sqlite3",
                "awk",
                # The attacks of the policy of a page (ADR 0052): the suite of
                # `make mmu-smoke` is files of this image and not of a build
                # of its own.
                "mmu_text",
                "mmu_execstack",
                "mmu_kernel",
                "mmu_heap",
                "mmu_wx",
                "mmu_stray",
                "mmu_stack",
                "mmu_io",
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
    # The streams of the library: a byte given back with `ungetc` is the first
    # byte of the next `fread`, and a relative seek counts from the stream and
    # not from the descriptor (the block of read-ahead sits between them).
    # Both were wrong in 1.2.0; the program says so in one line.
    expect(
        "a stream reads a byte given back and seeks from the stream",
        "streams: ok" in answer("exec /bin/streams"),
    )
    # ftruncate (35): the program cuts a file of the volume, the bytes that
    # are left are the first ones, and the size the kernel reports is the
    # one of the call.
    cut = answer("exec /bin/truncate 4 /tmp/trunc")
    expect("ftruncate cuts a file to the size of the call", "truncate: /tmp/trunc is 4 bytes" in cut)
    left = answer("exec /bin/cat /tmp/trunc")
    expect(
        "the bytes left are the first of the file",
        "0123" in left and "0123456789" not in left,
        left,
    )
    expect(
        "the size after the cut is the one of the file",
        "4 /tmp/trunc" in answer("exec /bin/wc -c /tmp/trunc"),
    )
    expect(
        "ftruncate of a name that is not there is the error of the open",
        "truncate: /no/such/file: no such file or directory"
        in answer("exec /bin/truncate 1 /no/such/file"),
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
    # (ADR 0017), and the prompt leaves the terminal of the task on it while
    # nothing is open (ADR 0051 §2): a redirect, a pipe or /dev/null is what
    # makes the read end.
    cat = answer("exec /bin/cat < /dev/null")
    expect(
        "cat reads descriptor 0 when no file is named",
        "bad file descriptor" not in cat,
        cat,
    )
    expect("cat of an empty input ends with 0", "the program exited with 0" in cat, cat)
    wc = answer("exec /bin/wc -l - < /dev/null")
    expect("wc reads descriptor 0 for a dash",
           "bad file descriptor" not in wc and re.search(r"(?m)^\s*0\s", wc) is not None, wc)
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
    expect(
        "the licenses of the ports travel with the image",
        all(name in licenses for name in ("zlib", "lua", "zstd", "file", "sqlite", "awk")),
        licenses,
    )

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
    lua_format = answer("exec /bin/lua -e 'print(string.format(\"%.3f\",1/3))'")
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

    # --- the third port of somebody else's program: awk and /bin/awk
    # The interpreter is a file of the image and its data and script are files
    # of /etc. The formatter, the fields and the arithmetic are the ones of
    # the library of the guest: a number with three places, a sum of a column,
    # and the OFS of a print.
    awk_format = answer('exec /bin/awk "BEGIN{printf\\"%.2f\\n\\",1/3}"')
    expect(
        "awk prints three places of a fraction",
        "0.33" in awk_format and "the program exited with 0" in awk_format,
        awk_format,
    )
    awk_fields = answer("exec /bin/awk -F: {print$1} /etc/columns")
    expect(
        "awk splits a record into fields",
        re.search(r"(?m)^root\r?$\n^bin\r?$\n^nik\r?$", awk_fields) is not None,
        awk_fields,
    )
    awk_sum = answer("exec /bin/awk -F: {s+=$3}END{print(s)} /etc/columns")
    expect(
        "awk sums a column of the data",
        re.search(r"(?m)^1001\r?$", awk_sum) is not None,
        awk_sum,
    )
    awk_ofs = answer("exec /bin/awk -v OFS=, -F: {print$1,$2} /etc/columns")
    expect(
        "awk joins two fields with OFS",
        re.search(r"(?m)^root,0\r?$", awk_ofs) is not None,
        awk_ofs,
    )
    # The script of the machine is a file of the image: `-f` reads it through
    # the VFS, BEGIN and END run, and the recount of the file is the answer.
    awk_script = answer("exec /bin/awk -f /etc/hello.awk /etc/columns")
    expect(
        "awk runs the script of /etc",
        "awk: hello from /etc/hello.awk" in awk_script
        and "awk: 3 records, sum 1001, root 1" in awk_script
        and "the program exited with 0" in awk_script,
        awk_script,
    )
    awk_gsub = answer('exec /bin/awk "{gsub(/o/,\\"0\\");print}" /etc/setting')
    expect(
        "awk substitutes every match of a pattern",
        "hell0" in awk_gsub and "h0stname=x86_64" in awk_gsub and "c0ns0le=vga" in awk_gsub,
        awk_gsub,
    )
    awk_getline = answer('exec /bin/awk "BEGIN{while((getline<\\"/etc/setting\\")>0)n++;print(n)}"')
    expect(
        "awk reads a file with getline",
        re.search(r"(?m)^3\r?$", awk_getline) is not None,
        awk_getline,
    )
    awk_width = answer('exec /bin/awk "BEGIN{printf\\"%5.2f|%d|%s\\n\\",3.14159,42,\\"s\\"}"')
    expect(
        "awk honours the width and the precision of a conversion",
        " 3.14|42|s" in awk_width,
        awk_width,
    )
    awk_star = answer('exec /bin/awk "BEGIN{printf\\"%*.*f|%i|%e\\n\\",8,2,3.14159,42,1234.5}"')
    expect(
        "awk takes the width and the precision from the arguments",
        "    3.14|42|1.234500e+03" in awk_star,
        awk_star,
    )
    awk_char = answer('exec /bin/awk "BEGIN{printf\\"%c%c|%x|%o\\n\\",65,66,255,8}"')
    expect(
        "awk prints a character, a hexadecimal and an octal",
        "AB|ff|10" in awk_char,
        awk_char,
    )
    awk_v = answer("exec /bin/awk -v x=42 BEGIN{print(x+1)}")
    expect("awk sets a variable with -v", re.search(r"(?m)^43\r?$", awk_v) is not None, awk_v)
    # The environment of the kernel reaches ENVIRON, which is the vector of
    # the call and not a copy of the shell.
    awk_env = answer('exec /bin/awk "BEGIN{print(ENVIRON[\\"PATH\\"])}"')
    expect("awk reads the environment of the machine", re.search(r"(?m)^/bin\r?$", awk_env) is not None, awk_env)
    awk_split = answer('exec /bin/awk "BEGIN{n=split(\\"a:b:c\\",p,\\":\\");print(n,p[2])}"')
    expect("awk splits a string into an array", re.search(r"(?m)^3 b\r?$", awk_split) is not None, awk_split)
    awk_strings = answer(
        'exec /bin/awk "BEGIN{print(substr(\\"hello\\",2,3),index(\\"hello\\",\\"ll\\"),length(\\"hello\\"))}"'
    )
    expect(
        "awk takes a substring, an index and a length",
        re.search(r"(?m)^ell 3 5\r?$", awk_strings) is not None,
        awk_strings,
    )
    awk_case = answer('exec /bin/awk "BEGIN{print(toupper(\\"abc\\"),tolower(\\"DEF\\"))}"')
    expect(
        "awk folds the case of a string",
        re.search(r"(?m)^ABC def\r?$", awk_case) is not None,
        awk_case,
    )
    # The arithmetic of the library of the guest: the x87 unit of the program,
    # and the formatter with six places of %g and three of %.3f.
    awk_math = answer(
        "exec /bin/awk BEGIN{print(sqrt(2),exp(1),log(10),sin(0),cos(0),atan2(1,1))}"
    )
    expect(
        "awk computes a root, an exponential, a logarithm and an angle",
        "1.41421 2.71828 2.30259 0 1 0.785398" in awk_math,
        awk_math,
    )
    awk_root = answer('exec /bin/awk "BEGIN{printf\\"%.3f\\n\\",sqrt(2)}"')
    expect("awk prints three places of a root", "1.414" in awk_root, awk_root)
    awk_floats = answer('exec /bin/awk "BEGIN{printf\\"%f|%g|%.2e\\n\\",1/3,1/3,1/3}"')
    expect(
        "awk prints a double as %f, %g and %e",
        "0.333333|0.333333|3.33e-01" in awk_floats,
        awk_floats,
    )
    awk_sprintf = answer('exec /bin/awk "BEGIN{s=sprintf(\\"%.2f\\",1/3);print(s)}"')
    expect("awk builds a string with sprintf", "0.33" in awk_sprintf, awk_sprintf)
    awk_int = answer("exec /bin/awk BEGIN{print(int(3.9),int(-3.9),3%2,2^10,2**10)}")
    expect(
        "awk truncates, takes a remainder and a power",
        re.search(r"(?m)^3 -3 1 1024 1024\r?$", awk_int) is not None,
        awk_int,
    )
    awk_match = answer('exec /bin/awk "BEGIN{s=\\"hello\\";print(match(s,/ll/),RSTART,RLENGTH)}"')
    expect(
        "awk reports where a pattern matched",
        re.search(r"(?m)^3 3 2\r?$", awk_match) is not None,
        awk_match,
    )
    # A field is a numeric string: the numbers of the third column compare as
    # numbers (0, 0, 1 against 9), which a string compare of "1000" and "9"
    # would answer the other way for the third line.
    awk_numeric = answer("exec /bin/awk -F: {print($3>9)} /etc/columns")
    expect(
        "a field compares as a number when the other side is one",
        re.findall(r"(?m)^([01])\r?$", awk_numeric) == ["0", "0", "1"],
        awk_numeric,
    )
    awk_rand = answer('exec /bin/awk "BEGIN{srand(1);r=rand();print(r>=0&&r<1)}"')
    expect(
        "awk rand answers a number below one",
        re.search(r"(?m)^1\r?$", awk_rand) is not None,
        awk_rand,
    )
    awk_exit = answer("exec /bin/awk BEGIN{exit(3)}")
    expect(
        "the status of an awk script is the status of its exit",
        "the program exited with 3" in awk_exit,
        awk_exit,
    )
    # The honest refusal of the two calls of a shell: the sentence is on the
    # error descriptor and the value of the call is -1, which is the failure
    # POSIX gives a command that cannot run. The program itself ends with 0.
    awk_system = answer('exec /bin/awk "BEGIN{print(system(\\"true\\"))}"')
    expect(
        "awk system refuses with a sentence and -1",
        "awk: system: this machine has no shell" in awk_system
        and re.search(r"(?m)^-1\r?$", awk_system) is not None
        and "the program exited with 0" in awk_system,
        awk_system,
    )
    awk_pipe = answer('exec /bin/awk "BEGIN{print(\\"echo\\"|getline)}"')
    expect(
        "awk cmd | getline runs the command through popen",
        re.search(r"(?m)^1\r?$", awk_pipe) is not None
        and "the program exited with 0" in awk_pipe,
        awk_pipe,
    )
    awk_lines = answer(
        'exec /bin/awk "BEGIN{while((\\"cat /etc/setting\\"|getline line)>0)n++;print(n)}"'
    )
    expect(
        "awk reads a whole file through the pipe of popen",
        re.search(r"(?m)^3\r?$", awk_lines) is not None
        and "the program exited with 0" in awk_lines,
        awk_lines,
    )
    awk_missing = answer("exec /bin/awk -f /etc/no-such.awk")
    expect(
        "a script that is not there is the sentence of the interpreter",
        "can't open file /etc/no-such.awk" in awk_missing and "exited with 2" in awk_missing,
        awk_missing,
    )
    awk_stdin = answer("exec /bin/awk {print} < /etc/setting")
    expect(
        "awk without a file reads descriptor 0",
        "hostname=x86_64" in awk_stdin and "i/o error" not in awk_stdin,
        awk_stdin,
    )

    # A program starts another program with an environment of its own. The
    # two words are the vector of `spawn.c`, copied by the kernel at the edge
    # and placed in the memory of the second program: the entry prints them.
    spawn = answer("exec /bin/spawn")
    expect(
        "a program starts another with its own environment",
        "PROGRAM: envp 2, PATH=/custom" in spawn,
    )
    expect("the started program gets its arguments", "PROGRAM: argc 2, argv[0] hello" in spawn)
    expect(
        "the shell sees the program that replaced spawn finish",
        "the program has finished" in spawn,
    )

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
    # --- the quotes of the lexer (1.2.1)
    # A word of the line is read before the operators are: the file the quoted
    # `echo` wrote holds the text as one word, and the pattern of `grep` and
    # the script of `sed` reach the program with their spaces whole.
    expect(
        "a quoted word keeps its space through a redirect",
        re.search(r"(?m)^a b\r?$", answer("cat /tmp/q")) is not None,
        repr(answer("cat /tmp/q")),
    )
    expect(
        "the pattern of grep is one word",
        re.search(r"(?m)^a b\r?$", answer("grep 'a b' /tmp/q")) is not None,
        repr(answer("grep 'a b' /tmp/q")),
    )
    expect(
        "the script of sed is one word",
        re.search(r"(?m)^c d\r?$", answer("sed 's/a b/c d/' /tmp/q")) is not None,
        repr(answer("sed 's/a b/c d/' /tmp/q")),
    )
    expect(
        "echo keeps the two spaces of a quoted run",
        "a  b\r\n" in answer("echo 'a  b'") and "a  b\r\n" in answer('echo "a  b"'),
        repr(answer('echo "a  b"')),
    )
    expect(
        "a pipe inside the quotes is a character",
        re.search(r"(?m)^a \| b\r?$", answer("echo 'a | b'")) is not None,
        repr(answer("echo 'a | b'")),
    )
    expect(
        "a redirect inside the quotes is a character",
        re.search(r"(?m)^x > y\r?$", answer('echo "x > y"')) is not None,
        repr(answer('echo "x > y"')),
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
        "the x bit is not for this caller" in answer("exec /bin/hello", 0),
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

    # --- the processes of a program: the POSIX pair of M1
    #
    # A program now has the POSIX fork/execve/waitpid: the child of a fork is a
    # task with a copy of the memory, execve of a program replaces the image
    # of the caller, and waitpid reaps the child and hands back its status.
    procs = answer("exec /bin/procs")
    expect(
        "a child of a program computes and returns its status",
        "PROGRAM: child computed 55" in procs
        and "PROGRAM: first child status 55" in procs,
        procs,
    )
    expect(
        "the parent of a program gets the pid of its child",
        re.search(r"PROGRAM: the parent got the pid [1-9][0-9]*", procs) is not None,
        procs,
    )
    expect(
        "a double fork hands two statuses up",
        "PROGRAM: middle waited, status 3" in procs
        and "PROGRAM: double fork status 4" in procs,
        procs,
    )
    # The child called execvp("/bin/echo"): if the call had started another
    # task and returned, the child would have printed its line after the call
    # and ended with 9, and the status would not be 0.
    expect(
        "an execve of a program replaces the child that called it",
        "PROGRAM: the exec child" in procs
        and "PROGRAM: execve returned" not in procs
        and "PROGRAM: exec child status 0" in procs,
        procs,
    )
    expect(
        "the program of the processes ends with 0",
        procs.count("PROGRAM: procs ends") == 1
        and "the program exited with 0" in procs
        and "the program has finished" in procs,
        procs,
    )

    # --- the floating point state of a task (K9)
    #
    # The parent and its two children keep a pair of doubles in the XMM
    # registers across a loop of three million iterations each, and the
    # scheduler switches between them many times in the middle of it. The
    # arithmetic is exact (a multiplication by two and by one half), so the
    # three lines are the values of three sets of registers: a kernel that
    # saves the general registers and not the floating point unit lets the
    # tasks read each other's numbers, and one of the lines is wrong.
    fpu = answer("exec /bin/fpu2")
    expect(
        "the parent keeps its own registers across the switches",
        "FPU: parent 1.000000000000 1.000000000000" in fpu,
        fpu,
    )
    expect(
        "the two children keep their own registers as well",
        "FPU: child 3 3.000000000000 0.333333333333" in fpu
        and "FPU: child 7 7.000000000000 0.142857142857" in fpu,
        fpu,
    )
    # The third child dirties the unit and calls execve of this same image
    # with `state`: a new image begins after fninit, so the control word is
    # 037f and MXCSR is 1f80 whatever the old image left.
    expect(
        "a new image of execve starts with the clean state",
        "FPU: state 037f 1f80" in fpu,
        fpu,
    )
    expect(
        "the children of the FPU test all finish",
        "FPU: all children ended, 0 failure(s)" in fpu
        and "the program exited with 0" in fpu,
        fpu,
    )

    # --- a pipeline deeper than the table of three
    deep = answer("cat /etc/setting | cat | cat | cat | cat | cat | cat | cat | cat | wc -l")
    expect(
        "a pipeline of ten stages carries the bytes to its end",
        re.search(r"(?m)^3\r?$", deep) is not None,
        deep,
    )
    expect(
        "every stage of a pipeline of ten is gone before the prompt",
        "cannot start stage" not in deep and "too many stages" not in deep,
        deep,
    )

    # The maximum of the documentation: fifteen stages of programs, sixteen of
    # the commands of the shell, and a chain of sixteen programs refused with
    # the arithmetic of the task table before anything is started.
    longest = answer(
        "cat /etc/setting | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | "
        "cat | wc -l"
    )
    expect(
        "a pipeline of fifteen stages of programs carries the bytes to its end",
        re.search(r"(?m)^3\r?$", longest) is not None,
        longest,
    )
    expect(
        "the last stage of fifteen writes its answer (the output is descriptor 1)",
        "bad file descriptor" not in longest and "cannot start stage" not in longest,
        longest,
    )
    sixteen = answer(
        "echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | echo | "
        "echo | echo | echo sixteen"
    )
    expect(
        "a pipeline of sixteen stages of the shell runs",
        "sixteen" in sixteen and "too many" not in sixteen.replace("too many words", ""),
        sixteen,
    )
    refused = answer(
        "cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat"
    )
    expect(
        "a chain of sixteen programs is refused with the arithmetic of the task table",
        "16 stages need 32 task slots and 30 of 32 are free" in refused,
        refused.strip(),
    )
    expect(
        "the chain that was refused started no stage",
        "task: created 'pipe" not in refused,
        refused.strip(),
    )

    # --- xargs -P: the children of a program
    parallel = answer("echo one two three four five six | xargs -P 4 -n 1 echo done")
    expect(
        "xargs -P runs every command as a child",
        all(f"done {word}" in parallel for word in ("one", "two", "three", "four", "five", "six")),
        parallel,
    )
    expect(
        "xargs -P does not duplicate the commands of its copies",
        parallel.count("done ") == 6 and "execvp" not in parallel,
        parallel,
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
    # The same signal through the fast door of `syscall`: a handler entered
    # on the way out of a call is the proof that the frame of the second door
    # is the frame the signal code rewrites (ADR 0035).
    expect(
        "a handler runs on the way out of the fast door as well",
        "PROGRAM: self SIGINT through syscall returned 0, handler runs 1" in signals,
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
    # taken by the wait: EINTR is 4. The wait is a call of the fast door, so
    # the signal arrives while the task sleeps inside it (ADR 0035).
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

    # --- the guard page under the stack (ADR 0048, ADR 0052)
    # The stack of a program starts as 64 KiB and grows into a room of 1 MiB
    # (ADR 0052); the page under the room is the guard and it is never
    # mapped, so a recursion that runs past the limit of the task meets that
    # page and not the data of the program. The fault is a store into a page
    # that is not present (error 0x6: not present, write, ring 3) in the
    # guard page at 0x15eef000 - and the program never prints the line after
    # the recursion.
    stack = answer("exec /bin/crash stack")
    expect("the stack overrun program starts", "PROGRAM: crash overruns the stack" in stack)
    expect(
        "the guard page under the stack takes the program down",
        "the program faulted: vector 14 at 0x0000000015ee" in stack
        and "error 0x0000000000000006" in stack,
        stack[-400:],
    )
    expect(
        "the program that overran its stack did not continue",
        "PROGRAM: crash did not fault on the stack" not in stack,
    )
    expect(
        "the shell survives the overflow of a program",
        re.search(r"MicroOS \d+\.\d+\.\d+", answer("uname", 1)) is not None,
    )
    # The task of the kernel has the page too: the command starts a task that
    # recurses past its own stack, waits for it and reports the status the
    # kernel gives a task that faulted. The machine stays.
    guard = answer("guard")
    expect(
        "a task that overflows its kernel stack dies and the machine lives",
        "guard: the task ran into the guard page and died (status 255); the machine is alive"
        in guard,
        guard[-400:],
    )
    expect(
        "the log names the task and the page of the overflow",
        "reached the guard page at 0x" in guard and "the task ends, the machine stays" in guard,
        guard[-500:],
    )
    # Every program of the run is audited before it starts: the report is a
    # line of the log, and the number of the pages that are both writable and
    # executable is zero for a map of this port. The line is short because it
    # is written before the entry of a program of the desktop, and the UART
    # of the machine holds the entry for the length of a line.
    audited = output.count("x86-64: the map of a program is W^X")
    finished = output.count("the program has finished")
    expect(
        "every program is reported W^X before it runs",
        audited >= 10 and audited >= finished + 1,
        f"{audited} audit(s) for {finished} line(s) of a program that finished",
    )
    # The audit is a refusal and not a remark (ADR 0052): a space with a page
    # that is writable and executable at once is not entered, and the line of
    # the refusal in a run of the whole image is the failure of the port. The
    # number of the audit lines is not the number of the programs here - the
    # run does not exec every file of /bin - so `make mmu-smoke` is the check
    # that counts them against its own execs.
    expect(
        "no program of the run was refused as not W^X",
        "the map of the program is not W^X" not in output,
        output[-600:],
    )

    # --- the first lane of 1.6: the wait for a descriptor
    poll = answer("exec /bin/poll")
    expect("poll sees the state of two empty pipes", "poll: empty, ready=0" in poll, poll[-400:])
    expect(
        "poll finds the byte of the second pipe only",
        "poll: one byte, ready=1" in poll and "poll: the second entry says 1" in poll,
    )
    expect(
        "poll ends a wait at its deadline",
        "poll: timeout, ready=0" in poll
        and re.search(r"poll: timeout, ms=(1[2-9][0-9]|[2-9][0-9][0-9])", poll) is not None,
        poll[-200:],
    )
    expect(
        "a poll of no descriptor is a sleep",
        "poll: sleep, ready=0" in poll
        and re.search(r"poll: sleep, ms=([6-9][0-9]|[1-9][0-9][0-9])", poll) is not None,
        poll[-200:],
    )
    # The wait is the scheduler's: the byte is written by a child 80 ms after
    # the poll began, and the answer comes when it arrives and not at a
    # deadline - the call had none.
    wait = re.search(r"poll: wait, ms=(\d+)", poll)
    expect(
        "poll wakes on the byte of another task",
        "poll: wait, ready=1" in poll and "poll: z" in poll,
        poll[-200:],
    )
    expect(
        "the wake is the child's and not a timeout",
        wait is not None and 60 <= int(wait.group(1)) < 400,
        wait.group(0) if wait else "no line",
    )
    expect(
        "the end of a pipe is readable",
        "poll: end of file, ready=1" in poll and "poll: end of file says 1" in poll,
    )
    expect(
        "a writing end whose reader is gone is an error",
        "poll: broken pipe says 8" in poll,
        "POLLERR is the error of the object",
    )
    expect("a number that is not open is an error", "poll: bad descriptor says 8" in poll)
    expect("the poll program ends with its status", "poll: done" in poll and "the program exited with 0" in poll)

    # --- the memory a program maps
    mapping = answer("exec /bin/mmap")
    size = re.search(r"mmap: file bytes (\d+)", mapping)
    through_mmap = re.search(r"mmap: sum through mmap (-?\d+)", mapping)
    through_read = re.search(r"mmap: sum through read (-?\d+)", mapping)
    expect("a file of the volume is mapped", size is not None and int(size.group(1)) > 0, mapping[:300])
    expect(
        "the bytes of the mapping are the bytes of the file",
        through_mmap is not None
        and through_read is not None
        and through_mmap.group(1) == through_read.group(1),
        f"{through_mmap.group(1) if through_mmap else '-'} against"
        f" {through_read.group(1) if through_read else '-'}",
    )
    # A private page is a copy: the write is in the memory of the program and
    # the file keeps its own byte. A shared page is the file: what is written
    # is what the next reader of the file sees.
    expect(
        "a private page takes a write the file does not",
        "mmap: the private page begins with 97" in mapping
        and "mmap: the file still begins with 97" in mapping,
    )
    expect("a shared page is written back by msync", "mmap: the file says HELLO" in mapping)
    expect("an anonymous page reads as zeros", "mmap: the anonymous page begins with 0" in mapping)
    expect("the heap grows by four pages", "mmap: the heap grew by four pages" in mapping)
    # The heap may not cross the stack of the program: the answer is ENOMEM
    # (12) and not a page of the stack overwritten.
    expect(
        "a heap past the stack is refused with ENOMEM",
        "mmap: the heap past the stack says 12" in mapping,
        mapping[-300:],
    )
    expect("the mmap program ends with its status", "mmap: done" in mapping and "the program exited with 0" in mapping)

    # The file the mapping wrote, read by another program: the first line of
    # the file is the five bytes of the shared page.
    written = answer("head -n 1 /mm.txt")
    expect("another program reads the shared write", "HELLO" in written, written[-200:])
    expect("the mapped file keeps its size", "524288 /mm.txt" in answer("wc -c /mm.txt"))

    # --- the memory of a run of a program of 4 MiB
    before = re.search(r"kernel heap: (\d+) KB free", answer("cat /proc/meminfo", 0))
    big = answer("exec /bin/big")
    big_line = re.search(r"big: (\d+) bytes, heap free before (\d+) KB, during (\d+) KB", big)
    after = re.search(r"kernel heap: (\d+) KB free", answer("cat /proc/meminfo", 1))
    expect("a program of 4 MiB runs", big_line is not None and "big: done" in big, big[:300])
    if big_line is not None:
        # Page cache keeps file pages in kernel heap across exec/read; the
        # cache is bounded and evictable, so heap deltas up to the cache
        # capacity are expected and are not a leaked program image.
        cache_slack_kb = 8192
        expect(
            "the image of a program is not copied into the heap",
            abs(int(big_line.group(3)) - int(big_line.group(2))) <= 64 + cache_slack_kb,
            f"{big_line.group(2)} KB before, {big_line.group(3)} KB during",
        )
        expect(
            "the memory of the program goes back with it",
            after is not None
            and abs(int(after.group(1)) - int(big_line.group(3))) <= 256 + cache_slack_kb,
            f"{big_line.group(3)} KB during, {after.group(1) if after else '-'} KB after",
        )
        expect(
            "the heap of the kernel where it was before the program",
            before is not None
            and abs(int(before.group(1)) - int(big_line.group(2))) <= 64 + cache_slack_kb,
            f"{before.group(1) if before else '-'} KB before the run",
        )

    return checks


# --- the accounts and the console (ADR 0033) -----------------------------
#
# The login of this machine is not a program of /bin that starts a shell: the
# console is the shell of the kernel and it starts as root, so `login` asks
# for the name and the password and hands the running shell to that user. A
# run of the shell by the list of commands cannot answer a prompt that is not
# the prompt of the shell, so this part of the smoke test boots the machine a
# second time and answers the console with a script: each step waits for its
# text and types its line, and a password is never echoed.
USERS_IMAGE = os.path.join(BUILD, "users-smoke.img")

# The script of the console: (the text to wait for, the line to type).
USERS_SCRIPT = [
    (PROMPT, "cat /etc/passwd"),
    (PROMPT, "cat /etc/shadow"),
    (PROMPT, "ls -l /etc/shadow"),
    (PROMPT, "id"),
    # The right password after a wrong one: the first answer fails and the
    # second opens the session, which is the three attempts of `login`.
    (PROMPT, "login root"),
    ("password: ", "wrong"),
    ("login: authentication failure", ""),
    ("password: ", "microos"),
    (PROMPT, "id"),
    # `su - guest` without a password, from root, and the directory of the
    # account. The session is the one task of the console, so `who` names it
    # as guest, and `whoami` is the name of the account and nothing else.
    (PROMPT, "su - guest"),
    (PROMPT, "pwd"),
    (PROMPT, "whoami"),
    (PROMPT, "who"),
    (PROMPT, "id"),
    # A user does not become root without the password of root.
    (PROMPT, "su - root"),
    ("password: ", "wrong"),
    (PROMPT, "id"),
    # The file a user makes is the user's (the debt of 1.6 is paid).
    (PROMPT, "echo seven > /tmp/seven.txt"),
    (PROMPT, "stat /tmp/seven.txt"),
    (PROMPT, "id"),
    # The password of root does open the way back.
    (PROMPT, "su root"),
    ("password: ", "microos"),
    (PROMPT, "id"),
    # An account without a password: three failures and the door closes.
    (PROMPT, "login guest"),
    ("password: ", "guest"),
    ("password: ", "guest"),
    ("password: ", "guest"),
    (PROMPT, "id"),
    # A login with no name: the empty answer is a name that is needed, and not
    # a name that does not fit, and it returns to the prompt of the shell.
    (PROMPT, "login"),
    ("login: ", "<enter>"),
    ("login: a name is needed", ""),
    (PROMPT, "id"),
    # `logout` ends the session of the console and the init starts another
    # one as root: that is the way back from a user, and `who` says it.
    (PROMPT, "whoami"),
    (PROMPT, "su - guest"),
    (PROMPT, "logout"),
    ("MicroOS shell ready", ""),
    (PROMPT, "id"),
    (PROMPT, "whoami"),
    (PROMPT, "who"),
    # --- the group and the setuid bit of the image (ADR 0044, ADR 0053)
    #
    # A file of the group 7 of root: the root session makes it and gives it to
    # the group, and the guest of the next block - the account of gid 7 - reads
    # it with the triad of the group and does not write it.
    (PROMPT, "mkdir /tmp/g"),
    (PROMPT, "write /tmp/g/team.txt seven"),
    (PROMPT, "chgrp 7 /tmp/g/team.txt"),
    (PROMPT, "chmod 640 /tmp/g/team.txt"),
    (PROMPT, "stat /tmp/g/team.txt"),
    # The session is a guest again: the group of the shell has a name, and the
    # two images that ask for the identity of their owner do and do not work
    # for a person. `/bin/suidcat` is the image of `cat` with the flag of the
    # header and the owner root, and `/bin/badcat` is the same image of the
    # mode 0777: a setuid program the caller may write, which execve refuses.
    (PROMPT, "su - guest"),
    (PROMPT, "groups"),
    (PROMPT, "stat /bin/suidcat"),
    (PROMPT, "exec /bin/suidcat /etc/shadow"),
    (PROMPT, "exec /bin/cat /etc/shadow"),
    (PROMPT, "exec /bin/badcat /etc/shadow"),
    (PROMPT, "id"),
    (PROMPT, "write /bin/suidcat x"),
    (PROMPT, "cat /tmp/g/team.txt"),
    (PROMPT, "write /tmp/g/team.txt x"),
    (PROMPT, ""),
]

# The answers of the script, one per line typed at the prompt of the shell. A
# step that is not the prompt of the shell (a password) is answered by the
# prompt that follows it, so the blocks are still one per command.
USERS_COMMANDS = [line for wait, line in USERS_SCRIPT if wait == PROMPT and line != ""]


def run_console(loader: str, kernel: str, image: str, timeout: float, steps: list) -> str:
    """Boot the machine and answer the console with the steps of a script.

    `steps` is a list of (text to wait for, line to type): the line goes out
    when its text appears, and the search for the next text starts after the
    one that was found, so a prompt that comes twice is answered twice. A step
    with an empty line only waits (the sentence of a refusal), and the word
    `<enter>` types the empty line a prompt waits for.
    """
    process = subprocess.Popen(
        machine_argv(loader, kernel, image),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    fd = process.stdout.fileno()
    transcript = b""
    cursor = 0
    step = 0
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline and step < len(steps):
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                break  # the machine went away on its own

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            while step < len(steps):
                index = text.find(steps[step][0], cursor)
                if index < 0:
                    break
                cursor = index + len(steps[step][0])
                if steps[step][1] == "<enter>":
                    # The empty line a prompt waits for: `login` with no name,
                    # a question answered by Enter.
                    process.stdin.write(b"\n")
                    process.stdin.flush()
                elif steps[step][1] != "":
                    process.stdin.write((steps[step][1] + "\n").encode("ascii"))
                    process.stdin.flush()
                step += 1
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
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


def users_checks(loader: str, kernel: str, image: str, timeout: float) -> list:
    """The accounts of /etc, `login`, `su` and the file a user makes."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # The script changes the volume (a file in /tmp), so the run gets a copy
    # of the image the build made, as the run of the commands does.
    shutil.copyfile(image, USERS_IMAGE)

    try:
        output = run_console(loader, kernel, USERS_IMAGE, timeout, USERS_SCRIPT)
    finally:
        if os.path.exists(USERS_IMAGE):
            os.remove(USERS_IMAGE)

    blocks = output.split(PROMPT)[1:]

    def answer(command: str, occurrence: int = 0) -> str:
        indexes = [index for index, line in enumerate(USERS_COMMANDS) if line == command]
        if occurrence >= len(indexes):
            return ""
        index = indexes[occurrence]
        return blocks[index] if index < len(blocks) else ""

    def last_line(text: str) -> str:
        """The answer of a command, without the echo of the line that asked."""
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        return lines[-1] if lines else ""

    expect(
        "the console answers the whole script",
        len(blocks) > len(USERS_COMMANDS),
        f"{len(blocks)} block(s) for {len(USERS_COMMANDS)} command(s)",
    )

    # The two files of the accounts are files of the disk, and the hash is the
    # salted SHA-256 the tree computes.
    passwd = answer("cat /etc/passwd")
    expect(
        "the accounts are the file of /etc/passwd",
        "root:0:0:/:/bin/sh" in passwd and "guest:7:7:/home/guest:/bin/sh" in passwd,
        passwd,
    )
    shadow = answer("cat /etc/shadow")
    expect(
        "the hash of /etc/shadow is a salted SHA-256",
        "root:$sha256$mox1$da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c"
        in shadow,
        shadow,
    )
    expect("an account without a password is locked", "guest:*" in shadow, shadow)
    expect(
        "only the owner reads /etc/shadow",
        "-rw-------" in answer("ls -l /etc/shadow") and "root" in answer("ls -l /etc/shadow"),
        answer("ls -l /etc/shadow"),
    )
    expect(
        "the shell starts as root",
        "uid=0(root) gid=0" in answer("id", 0),
        answer("id", 0),
    )

    # `login`: a wrong password and then the right one, and the task stays
    # root because root was the name.
    login = answer("login root")
    expect(
        "login refuses a wrong password and takes the right one",
        "login: authentication failure" in login and "login: root, uid 0, gid 0" in login,
        login,
    )
    expect(
        "a login of root leaves the shell root",
        "uid=0(root)" in answer("id", 1),
        answer("id", 1),
    )
    # `su - guest` from root: no password, the credentials of the account and
    # its home directory.
    expect(
        "su - enters the home of the account",
        answer("pwd").strip().endswith("/home/guest"),
        answer("pwd"),
    )
    expect(
        "su gives the shell the uid and the gid of the account",
        "uid=7(guest) gid=7(guest) home=/home/guest" in answer("id", 2),
        answer("id", 2),
    )
    expect(
        "whoami names the guest of the session",
        last_line(answer("whoami", 0)) == "guest",
        answer("whoami", 0),
    )
    expect(
        "who lists the session of the console as its user",
        "guest" in answer("who", 0) and "console" in answer("who", 0),
        answer("who", 0),
    )

    # A user does not become root without the password of root: the wrong one
    # is refused and the shell is still guest.
    expect(
        "su refuses the wrong password of root",
        "su: authentication failure" in answer("su - root"),
        answer("su - root"),
    )
    expect(
        "a refused su leaves the shell as the user",
        "uid=7(guest)" in answer("id", 3),
        answer("id", 3),
    )

    # The file a user makes is owned by the user: the create path of the VFS
    # carries the uid of the task that made the file.
    stat = answer("stat /tmp/seven.txt")
    expect(
        "a file of a user is owned by the user",
        "owner: 7" in stat and "size: 6" in stat,
        stat,
    )
    expect(
        "the user is still the user after writing the file",
        "uid=7(guest)" in answer("id", 4),
        answer("id", 4),
    )

    # The password of root is the way back up.
    expect(
        "su root with the right password makes the shell root",
        "uid=0(root)" in answer("id", 5),
        answer("id", 5),
    )

    # An account with no password: three failures and nothing changes.
    login = answer("login guest")
    expect(
        "a locked account fails three times",
        login.count("login: authentication failure") == 3,
        login,
    )
    expect(
        "a refused login leaves the shell as it was",
        "uid=0(root)" in answer("id", 6),
        answer("id", 6),
    )

    # A login with no name: one sentence, which asks for a name.
    empty = answer("login")
    expect(
        "login with no name asks for a name and says nothing else",
        "login: a name is needed" in empty
        and "does not fit" not in empty
        and "no such account" not in empty,
        empty.strip(),
    )
    expect(
        "a login with no name changes nothing",
        "uid=0(root)" in answer("id", 7),
        answer("id", 7),
    )

    # The session of the console: `whoami` names the account, `who` lists the
    # session, and `logout` ends it. The init of the port starts another one
    # as root, which is the way back from a user of this system.
    expect(
        "whoami is root before the logout",
        last_line(answer("whoami", 1)) == "root",
        answer("whoami", 1),
    )
    expect(
        "logout ends the session",
        "logout: the session ends" in answer("logout"),
        answer("logout"),
    )
    expect(
        "the console starts again as root after a logout",
        "uid=0(root)" in answer("id", 8),
        answer("id", 8),
    )
    expect(
        "whoami is root after the logout",
        last_line(answer("whoami", 2)) == "root",
        answer("whoami", 2),
    )
    expect(
        "who names the new session of the console",
        "root" in answer("who", 1) and "console" in answer("who", 1),
        answer("who", 1),
    )

    # --- the group and the setuid bit of the image (ADR 0044, ADR 0053)
    #
    # The tail of the script is the group of a file and the two images that ask
    # for the identity of their owner. The commands are answered by their
    # occurrence in the script, which is the rule of the helper above.
    group_stat = answer("stat /tmp/g/team.txt")
    expect(
        "root gives a file to a group and the record keeps it",
        "mode: 0640" in group_stat and "group: 7" in group_stat,
        group_stat,
    )
    expect(
        "the account of the group reads the file of the group",
        re.search(r"(?m)^seven\r?$", answer("cat /tmp/g/team.txt")) is not None,
        answer("cat /tmp/g/team.txt"),
    )
    expect(
        "the account of the group does not write the file of the group",
        "not permitted" in answer("write /tmp/g/team.txt x").lower(),
        answer("write /tmp/g/team.txt x"),
    )

    expect(
        "groups names the group of the shell from /etc/group",
        last_line(answer("groups")) == "guest",
        answer("groups"),
    )

    suid_stat = answer("stat /bin/suidcat")
    expect(
        "the image of the setuid program is root's file of 0755",
        "mode: 0755" in suid_stat
        and "owner: 0" in suid_stat
        and "group: 0" in suid_stat,
        suid_stat,
    )

    # The program asks for the identity of its owner: it reads the file that
    # only root reads, and the same image without the bit is refused.
    suid_cat = answer("exec /bin/suidcat /etc/shadow")
    expect(
        "a setuid program reads a file of its owner and not of its caller",
        "$sha256$mox1$" in suid_cat,
        suid_cat.strip()[-300:],
    )
    plain_cat = answer("exec /bin/cat /etc/shadow")
    expect(
        "the same program without the bit is refused to the same caller",
        "not permitted" in plain_cat.lower() and "$sha256$" not in plain_cat,
        plain_cat.strip()[-300:],
    )

    # A setuid image that the caller may write is not a door: the call of
    # execve refuses it, and the line of the kernel says which file and why.
    bad_cat = answer("exec /bin/badcat /etc/shadow")
    expect(
        "a setuid image the caller may write is refused",
        "asks for the identity of its owner" in bad_cat
        and "not permitted" in bad_cat
        and "$sha256$" not in bad_cat,
        bad_cat.strip()[-300:],
    )

    # The identity was the program's and not the shell's: the task that asked
    # for the program is the guest it was. `id` is the tenth of the script -
    # the nine before it are the sessions of `login`, `su` and the logout -
    # and it reads the effective uid of the task.
    expect(
        "the shell keeps its own identity after a setuid run",
        "uid=7(guest)" in answer("id", 9),
        answer("id", 9),
    )

    # And the file of root is not a file of the caller.
    expect(
        "the setuid program cannot be written by its caller",
        "not permitted" in answer("write /bin/suidcat x").lower(),
        answer("write /bin/suidcat x"),
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
    # The machine reads the image of a program through the disk driver of the
    # port for every `exec`, and a read of a large file is the slowest thing
    # this machine does (docs/debts.md): the command line has programs of
    # 437 KiB (zstd) and 168 KiB (lua) in it, and a run of the list of
    # COMMANDS needs a minute of an emulated machine that is not the fastest.
    # The list of the commands of the image is the one of the tree of every
    # port: the tools of sbase, the interpreter of Lua, the command of zstd and
    # `/bin/file` with its database, each an image of a few hundred kilobytes
    # that the machine reads through the disk driver for every `exec`. Two
    # minutes was the limit of one port of somebody else, and the merged list
    # of two of them needs three times that on a machine that is not the
    # fastest: the limit is the whole run and a slow host is not a failure.
    parser.add_argument("--timeout", type=float, default=600.0, help="the seconds the machine gets")
    parser.add_argument(
        "--disk",
        choices=DISKS,
        default="ide",
        help="the bus of the disk of the run (default: ide)",
    )
    parser.add_argument(
        "--cpu",
        default="",
        help="the processor of the machine, e.g. qemu64,+smep,+smap (default: QEMU's own)",
    )
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    global DISK, CPU
    DISK = args.disk
    CPU = args.cpu

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
        + zstd_compat_checks(loader, kernel, args.timeout)
        + oversize_checks(loader, kernel, args.timeout)
        + users_checks(loader, kernel, args.image, args.timeout)
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
