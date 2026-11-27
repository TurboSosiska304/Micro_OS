# The shell

Every boot ends at a prompt:

```text
MicroOS shell ready. Type 'help' for the list of commands.
microos>
```

The shell is `components/kshell`, part of the kernel: the same interpreter and
the same commands on the board, in the simulator and on x86-64. A port or a
driver adds commands of its own by registering them by name, which is why the
list is not the same on every machine.

## The line

The line is split on spaces and tabs, and a word may be quoted or escaped:

* `'...'` is a literal: every character inside it is itself, even a space,
  a `|`, a `>` or a backslash;
* `"..."` groups the spaces and the operators, and inside it a backslash
  escapes `"` and `\` only: `"a\"b"` is `a"b`, and `"a\nb"` is the four
  characters;
* outside the quotes a backslash takes the character after it, so `a\ b` is
  one word and `\|` is a bar. A backslash at the end of the line is refused:
  there is no continuation of a line.

An empty word is a word: `echo ""` prints an empty line, and a command that
sees `argv[i] == ""` sees an empty argument (`path ""` still refuses it). A
quote that is not closed refuses the whole line with `unterminated quote`;
the shell never guesses where the word was meant to end.

A line is at most 127 characters and at most 8 words per stage. Backspace
erases, Ctrl+C throws the line away, and a line that did not fit is **not**
executed (the shell says `line too long, it was not executed`), because a cut
line is a different command. While a program or a pipe runs, Ctrl+C is not a
line: the shell reads it and sends `SIGINT` to the foreground job. Ctrl+Z
sends `SIGTSTP`, returns the terminal to the shell and leaves the job in the
table as `Stopped` (see [kernel-abi.md](kernel-abi.md#signals)).

A name that is not registered answers `<name>: command not found`. A command
that is given the wrong arguments answers with its own `usage:` line and does
nothing.

## Lists

`;`, `&&` and `||` join pipelines. `;` runs the next pipeline whatever the
last status was; `&&` runs it only after status 0; `||` runs it only after a
nonzero status. `&&` and `||` have the same precedence and associate to the
left. The whole line is parsed before any command runs, so a syntax error at
the end does not leave a prefix behind.

```text
microos> echo one; echo two
microos> cat /etc/setting && echo yes
microos> cat /no/such/file || echo recovered
microos> echo 'a && b ; c'
```

The list operators may touch their neighbours (`a;b`, `a&&b`), while a pipe
or a redirection still needs spaces around it. The status of a list is the
status of the last pipeline that actually ran; a skipped pipeline does not
change it.

## Pipes and redirections

`|` joins two or more stages, and `>`, `>>`, `<`, `2>`, `2>>` and `2>&1` send
a descriptor of a stage to a file or another descriptor. The operators are
words of their own, with a space around them, and they are read after the
quotes are taken off the line: a `|` or a `>` inside quotes is an ordinary
character of the argument. They may stand before or after the arguments of a
stage:

```text
microos> ls /etc | cat
microos> echo one | cat | cat
microos> cat /etc/setting > /out
microos> cat /etc/setting >> /out
microos> cat < /etc/setting
microos> cat /etc/setting 2> /errors
microos> cat /no/such/file > /errors 2>&1
microos> echo 'a | b' > /q
microos> grep 'a b' /q
```

On x86-64, where `/bin` holds the sbase programs, a pipeline can end in one
of them:

```text
microos> /bin/cat /etc/setting | wc -l
microos> ls /bin | wc -l
```

A stage that is a command of the shell (`echo`, `df`, `tree`, ...) runs in a
task of its own and writes descriptor 1, so a redirect or a pipe carries its
output exactly as it carries a program's. A name that is not a command is
looked up in `PATH` and started with `execve`; on x86-64 that program
inherits the three descriptors of the stage, which is what makes
`echo one | /bin/cat` and `ls /bin | wc -l` work.

The output redirection is installed before `2>&1`, so the duplicate names
the file or pipe the stage writes. A `2> file` is installed first, which lets
an error made while opening the output reach that error file.

The ten commands of sbase (`ls`, `cat`, `grep`, `head`, `tail`, `wc`, `sort`,
`find`, `sed`, `xargs`) are programs of the x86-64 disk, and the shell looks
their bare name in `PATH` **before** its own table: `ls /etc` at the prompt is
`/bin/ls`, the same file `exec /bin/ls /etc` runs. The port is asked first
(`k_port_exec_supported()`): the simulator cannot enter an image and keeps
the command of the shell, and the board has no `ls` of sbase in its `/bin`
and falls back the same way. A file that exists but cannot run is an error
and not a silent fallback.

What the line does **not** do:

* no `>&`, `1>&2` or general file-descriptor juggling: `2>&1` is the one
  duplicate the shell parses;
* no globs, variables or substitutions: `*` is a word and not a pattern, `$x`
  is the two characters, and backticks are not a command;
* no attached pipe or redirection: `a|b` and `>file` are arguments; write
  `a | b`, `> file`. List operators may be attached (`a;b`, `a&&b`);
* at most sixteen stages: the parser keeps sixteen and a line of 32 words
  cannot hold more. The pipes are made one boundary at a time as the stages
  are forked - the shell keeps the reading end between two forks and closes
  the boundary behind it - so its table holds two pipe ends and not
  eighteen; the system has sixteen pipes and forty open-file descriptions;
* but the task table is the real bound of a chain of programs. A stage that
  runs a program is two tasks of the kernel - the stage, which waits, and the
  program, which `execve` starts in a task of its own - and a stage that is a
  command of the shell is one; `init` and the shell are two more of the 32
  slots. On x86-64, where every stage of sbase is a program, **fifteen** stages
  fit (2·15 + 2 = 32) and sixteen do not (2·16 + 2 = 34); sixteen stages of
  commands of the shell fit. The shell counts the slots before it makes the
  first pipe and refuses a chain that does not fit with the numbers in the
  sentence, so no part of a line is started and then lost;
* `&` backgrounds one pipeline; the shell keeps four job entries. There is
  no POSIX subshell for a whole asynchronous AND-OR list, no `wait`, no job
  specification beyond `%N`, and no SIGKILL/TTIN/TTOU;
* a command that changes the task of the shell (`cd`, `path`, `jobs`, `fg`,
  `bg`) cannot be a stage or carry a redirection: it is refused with a
  sentence instead of changing a copy nobody sees.

The status of a line is the status of the last pipeline that ran, and the
status of a pipeline is the status of its last stage. A background launch
answers 0 without waiting; a stopped foreground job answers `128 + SIGTSTP`.
A program contributes its own exit code: the port waits for the program and
records that code for the kernel task that started it, while `execve()` keeps
its ABI answer of 0 on return. A program that could not start is an error.

## `help`

```text
microos> help
command   what it does
mkfs      format the volume: mkfs -y
...
programs in PATH:
  hello crash fork badptr signals cat echo ls mkdir mv edit kilo ...
microos> help exec
usage: exec <program> [arguments...]
run a program
```

`help` with no argument is the list of the commands of the shell, and below it
**every** program found in `PATH`: the executable files of the directories,
read from the directories themselves and not from a table. The list used to
stop at twenty-four names and a `...` for the screen of a small board, which
made the rest of the machine a secret of the repository (`/bin/edit` was never
named); the whole of it is printed now, and `make x86-smoke` reads `/bin` with
the shell and asks `help` for every name of it, so the two cannot drift apart.
A program is not a command of the shell and was missing from this list before
1.2.1. There is no `vi`: the editor is `kilo` in `/bin/ports` (or `/bin/edit`
in a window), and `exec /bin/vi` answers `no such file or directory`.

`help NAME` answers with the command of the shell, the program, or both. A
name that is only a program says where it is:

```text
microos> help hello
hello is a program at /bin/hello (type hello --help)
microos> help ls
ls is a program: /bin/ls
the builtin:
usage: ls [-la] [--color[=always|never|auto]] [path]
...
```

The first line names the file that runs at the prompt; the block below it is
the command of the shell, which answers on a machine whose `/bin` has no `ls`.
`help --help` explains `help` itself.

## Options and `-h` / `--help`

Every command answers `-h` and `--help` without running: a `usage:` line, the
summary from the table of the commands and the notes of the command. The flag
has to be the first word of the line; a `-h` deeper in the arguments is a
character the command sees. `--` ends the options, so a name that starts with
`-` can still be passed.

A command that does not have a flag refuses it: `ls -z` prints
`ls: unknown option: -z` and the usage line. `df -h` is the one GNU-shaped
exception: `-h` asks for human sizes, so the help of `df` is `df --help`.

## The commands

The core of the kernel, on every machine:

| Command | What it does |
|---|---|
| `help [command]` | the list of commands, or the full help of one |
| `uname` | the name, the version, the architecture and the build id |
| `uptime` | the seconds since the boot, the scheduler ticks and the task count |
| `ps` | the tasks: pid, priority, state, name |
| `free` | the memory of the heap and of the PSRAM |
| `dmesg` | the log of the boot, from the ring buffer of the kernel |
| `echo <words>` | print the words |
| `sleep <milliseconds>` | wait, up to 60000 |
| `jobs` | the shell's jobs: number, pid, process group, state and command |
| `fg [%N]` | continue a job in the foreground and wait for it |
| `bg [%N]` | continue a stopped job in the background |
| `clear` | clear the screen (the escape sequence the panel and the VGA read) |
| `modules` | run the hardware doctor and print its report |
| `abi` | the table of the system calls |
| `ulimit [-m bytes \| -c ms \| -n fds]` | the limits of the shell, or lower one of them |
| `memtest [kilobytes]` | measure the memory and its speed |
| `mfetch [color]` | the system report with a logo |
| `shutdown [-f] [off\|halt\|reboot]` | the clean end: no new session, the tasks end, the volumes are synced, the disks flushed, the mounts leave, then the state asked for |
| `poweroff [-f]`, `halt [-f]`, `reboot [-f]` | the same end with the power off, the processor stopped or the machine reset |

The files, on every machine:

| Command | What it does |
|---|---|
| `pwd`, `cd <dir>` | the current directory |
| `ls [-la] [--color[=when]] [path]` | the names of a directory, one per line; `-l` adds the mode, the owner, the size and the date, `-a` puts `.` and `..` first |
| `tree [-ad] [path]` | the levels below a path, with `` `-- `` branches; `-d` keeps the directories only |
| `cat [-n] [file]` | the content of a file, or of the standard input; `-n` numbers the lines (the command of the shell; on x86-64 the `cat` of `/bin` runs at the prompt and takes `-u`) |
| `write <file> <text>` | create or replace a file with one line (at most 127 bytes) |
| `mkdir <dir>...`, `rm [-r] <file>` | every word is a directory; a file; a directory needs `-r` |
| `df [-h] [path]` | the free blocks and inodes; `-h` in KB/MB. The block is the unit of the volume and says so (`1 KB each`, `512 bytes each`), and a volume that keeps no inodes (FAT32) says `not kept by this volume` |
| `mount` | the mounted filesystems |
| `fsck [-y] [fat <point\|device>] [ext2 <point>]` | check a volume: MicroFS, the FAT volume at a point or on a device, or the ext2 volume at a point; `-y` repairs it |
| `stat <path>` | the type, the mode, the owner, the group and the size |
| `chmod <mode> <path>`, `chown <uid> <path>`, `chgrp <gid> <path>` | the mode (octal), the owner and the group (0..32767) |
| `path [directories]` | show or set `PATH` |
| `exec <program> [args...]` | run a program, by a path or found in `PATH` |
| `kill [-signal] <pid>` | send `INT`, `TERM`, `CHLD`, `PIPE`, `STOP`, `CONT` or `TSTP` to a task; TERM by default. `init` and the console itself are refused with a sentence: there is no `getty`, so a signal to the shell would leave the machine without one |
| `login [name]` | open a session as a user of `/etc/passwd`: the name and the password, three attempts, the uid, the gid and the home of the account (ADR 0033) |
| `su [-l] [name]` | become another user: root without a password, a user with the password of the account; `-l` enters the home |
| `logout` | end the session of this terminal: the console starts another one as root, a client of telnet is disconnected |
| `who` | the sessions that are in: the name of `/etc/passwd`, the terminal and the pid (`/proc/sessions`) |
| `whoami` | the name of the account of the shell |
| `id` | the uid and the gid of the shell, the names of `/etc/passwd` and `/etc/group` for them, the home and the shell |
| `groups` | the names of the groups of the shell (one: a task has one gid) |

The services, where the port registers them:

| Command | What it does | Where |
|---|---|---|
| `config`, `config sync` | list the store of `/etc`, restore and save it | board, simulator; x86-64 has no store |
| `mkfs -y` | format the volume (erases everything) | board, simulator |
| `http get <url>` | one HTTP request; prints the status and the body | board, x86-64 |
| `wget <url> <file>` | download a URL into a file; a second run continues it | board, x86-64 |
| `pkg <operation> [flags] [name...]` | the installer: `-Syu`, `-S NAME`, `-U FILE`, `-R NAME`, `-Q` | x86-64 |
| `ntp [server]` | set the clock from NTP (UTC), default `pool.ntp.org` | board |
| `httpd [port\|off]` | the page of the state over HTTP, default port 80 | board |
| `mqtt <host> <port> <topic> <message>` | publish one message at QoS 0 | board |
| `telnet [on\|off]` | the shell on port 23 | board |
| `date`, `date set <YYYY-MM-DD> <HH:MM:SS>` | the wall clock of the kernel in UTC (the RTC of the board, the CMOS of x86-64, the clock of the host); `set` writes it through the kernel | every port |
| `battery`, `beep [frequency] [ms]`, `led on\|off\|heart\|dance` | the power cell, the buzzer, the LED | board, simulator (stubs) |
| `input [seconds]` | watch the buttons | board, simulator (stubs) |
| `wifi [status\|scan\|connect <ssid> [key]\|join\|disconnect\|forget]` | the WiFi station | board, simulator (stub) |
| `display [<x> <y>] \| rotate <0\|90\|180\|270> \| invert [on\|off] \| draw [seconds]` | the panel | board |
| `i2c` | scan the I2C bus | board |
| `mic` | the level of the microphone | board |
| `rtc alarm [HH:MM\|+<seconds>\|day <1-31> HH:MM\|clear]` | the alarm of the BM8563 and its flag | board |
| `power [sleep <seconds>\|off]` | deep sleep with the timer or button C, or the clean end and a deep sleep woken by button C | board |
| `factory reset -y` | erase MicroFS and the NVS, then restart; without `-y` it only explains | board |

The simulator has no network and no drivers: `http`, `wget`, `pkg`, `ntp`,
`httpd`, `mqtt` and `telnet` are not registered there at all, and the driver
commands of the board are stubs that answer something sensible. The x86-64
port registers the core and the files, and the services of its image: the
network of K6 (`http`, `wget`, `httpd`, `ntp`) and the installer (`pkg`)
arrive with `make x86-net-build`; an image without the network has none of
them. This is the port boundary doing its job, not a bug; [ports.md](ports.md)
has the whole matrix.

## The clock and the memory

The time of this system has one style: seconds since the boot, then the ticks
of the scheduler, and a word that matches the number. A tick is the period of
the scheduler, one millisecond; the seconds come from the microsecond clock of
the port and are the truth when a busy kernel misses a tick. `free` is the
memory and nothing else.

```text
microos> uptime
up 93 s, 93207 ticks, 5 tasks
microos> cat /proc/uptime
up 93 s, 93207 ticks
```

`memtest [kilobytes]` fills a block, sums it and copies it, for the internal
heap and the PSRAM one after the other. A single memset of 32 KB is shorter
than a tick of the x86-64 clock, so each operation is repeated until the clock
has moved at least a millisecond and the speed is the total bytes over the
total time. A clock that still cannot resolve the work says `too fast to
measure` instead of dividing by one microsecond:

```text
microos> memtest
memory test, block of 32 KB
internal heap: 318 KB total, 201 KB free, 117 KB used
  32 KB block: fill 65536 KB/s, sum 32768 KB/s, copy 65536 KB/s, data ok
psram: not available on this platform
```

## `PATH` and `exec`

`PATH` starts at `/bin:/bin/ports:/usr/bin` and belongs to the shell: it is not a file of
`/etc`, and a program does not inherit it yet. The commands of the shell that
change its state (`cd`, `path`) and the ones that have no program (`help`,
`uname`, `ps`, `df`, `mount`, `config`, `stat`, `write`, `tree`, `kill`, ...)
are always the shell's own. The ten names of sbase are the other way round:
the file wins, and the command is the fallback.

```text
microos> path
/bin:/bin/ports:/usr/bin
microos> path /bin
microos> path ""
path: an empty value is not a list of directories
```

A path is normalized before the call: `.` is the directory itself, `..` is
its parent, a run of slashes is one slash, and a trailing slash names the same
entry. `cd ..` from `/` stays at `/`, and `ls ..` is the parent of the current
directory; the filesystem of this system never sees a `..` in a path.

A name with a slash is a path and is used as it is. A name without one is
looked up in `PATH`, and the first regular file with an execute bit wins; a
file without the bit is passed over, as in Linux. A search that found nothing
says where it looked:

```text
microos> exec hello
exec: hello: not found in /bin:/bin/ports:/usr/bin
```

On the board and in the simulator there is no program to run: a fresh volume
has no `/bin`, and `execve` answers `ENOSYS` even for a file that is there. The
x86-64 port has `/bin` in the MicroFS volume of its disk image
(`make x86-disk`): `hello`, `crash`, `fork`, `badptr`, `signals`, `nx`,
`cat`, `echo`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`, `xargs`,
`sed`, `mkdir`, `rm`, `pwd`, `spawn` and `zcat` are files with the mode 0755,
and the boot and the shell run them as programs of ring 3. The library programs inherit the descriptors of the shell, so a
pipeline can end in one of them. [programs.md](programs.md) explains what a
program is.

## Jobs, Ctrl+C and Ctrl+Z

A pipeline runs in its own process group. A foreground pipeline takes the
terminal; the shell waits, polls Ctrl+C and Ctrl+Z, and restores its own
foreground group before it reads the next line. `&` starts the pipeline in a
new group without waiting and prints its number and leader pid:

```text
microos> sleep 3000 &
[1] 42
microos> jobs
[1] pid 42 pgrp 42 Running sleep 3000
```

The table belongs to the shell task and keeps four entries. `jobs` reaps
finished stages and prints `Running`, `Stopped` or `Done`, the pid, the process
group and the command. `fg %N` gives the group the terminal, sends `SIGCONT`
and waits; `bg %N` sends `SIGCONT` and leaves the group in the background.
Without `%N`, both use the most recent unfinished job.

Ctrl+Z sends `SIGTSTP` to the foreground group. The tasks leave the run queue
and the sleep table, the shell prints `^Z`, and the job stays in the table as
`Stopped` until `fg` or `bg` sends `SIGCONT`. Ctrl+C sends `SIGINT` to the
whole group. A program that reads its terminal answers the same keys through
the tty line discipline; at the shell prompt Ctrl+Z only throws the line away,
because there is no parent job table here to continue the shell.

`kill` takes a positive pid from `ps` and sends `TERM` unless a signal is
named: `-INT`, `-TERM`, `-CHLD`, `-PIPE`, `-STOP`, `-CONT`, `-TSTP` or the
number. A pid that names no task answers `kill: 42: no such process`. The
state-changing signals cannot be caught or ignored. There is no SIGKILL yet,
so a program that ignores SIGINT still needs `reboot` if it cannot be stopped
and removed.

## `ls` and `tree`

On x86-64 a bare `ls` is `/bin/ls` of sbase: the names of a directory, one per
line and sorted, without a slash at the end of a directory. `-l` adds the
kind and the nine bits of the mode, the owner, the group, the size and the
moment; `-a` puts `.` and `..` first; `--color[=always|never|auto]` paints a
directory blue and an executable green (`--color` is always, `auto` asks
`isatty`, which is 0 on this console). The path itself is not a line of the
answer — `tree` is the command that draws the levels.

The shell has an `ls` of its own for the machines without a program (the
simulator and the board): it prints the names one per line in the order of the
filesystem, a directory with a slash at its end, and its `-l` is a line per
entry with the mode, the owner, the size and the date. The listing has no
fixed table and no limit: every name is printed as it is read, and `-a` puts
`.` and `..` first. It takes `--color` too, with the same three colours and
the same sequences. The two are different commands on purpose: one is a
program of the disk, the other is the shell of the kernel.

```text
microos> ls /etc
fstab
init.d/
setting
microos> ls -l /etc
drwxr-xr-x root        14 21 Nov 19:03 init.d/
-rw-r--r-- root         6 21 Nov 19:03 setting
```

`--color` writes a directory in blue and a file with an execute bit in green;
the colours are the SGR sequences of a terminal, so a screen shows them and
the serial copy of the console does not. `--color=always` is the flag itself,
`--color=never` is the plain listing and `--color=auto` is the plain listing
as well, because this shell has no terminal to ask about. Without the flag the
names are plain, as they always were. A person reads the result on the screen
of x86-64: `make x86-vga-smoke` checks the colour of the cell of a directory
and of an executable out of the picture.

The owner 0 is written `root`, a greater uid as a number. A record without a
mode prints `?---------`; a record without a moment prints `-` in the date
column. A file is its name as it was given: `ls hello` answers `hello`.

`tree [-ad] [path]` draws the levels below a path with `|--` and `` `-- ``
branches and ends with the count of the entries. `-d` keeps the directories in
the drawing; the files are still counted, because the summary is the content
of the tree and not the branches that were drawn. `-a` is accepted for the
hidden names, of which this system has none yet. The walk stops at the sixth
level with `... (deeper)`, and a directory that cannot be read is skipped.

```text
microos> tree /etc
/etc
|-- init.d/
|   `-- clock
`-- setting
1 directory, 2 files
microos> tree -d /etc
/etc
`-- init.d/
1 directory, 2 files
```

The simulator has a script in `/etc/init.d` (`clock`); the x86-64 image
ships the directory empty and the same `/etc/setting`.

A path that does not exist is a sentence with the error of the filesystem and
the status of the command is not 0: `ls: /nope: no such file or directory`.

## `stat`, `chmod` and `chown`

Every object has a mode of nine bits and an owner, a uid of fifteen bits. A new
file is `0644`, a new directory `0755`, and the owner is 0. `stat` prints the
line of `ls -l` first and then the same numbers as fields:

```text
microos> stat /etc
drwxr-xr-x 0 14 /etc
type: directory
mode: 0755
owner: 0
size: 14
```

An object of a filesystem that keeps no modes (`/proc`, `/dev`, or a volume
written by an older release) prints `mode: (not set)` and no `ls` line: nine
dashes would read as `0000`, which is a statement the record does not make.

`chmod` takes an octal mode, `chown` a uid:

```text
microos> chmod 0700 /etc/init.d
microos> chown 0 /etc/setting
microos> chmod 0755 /bin/hello
```

A mode that is not octal (`0999`), a uid above 32767 and a missing path are
refused with a sentence. `chmod 000` is not a way to lock a file: the
filesystem keeps a zero as "this record predates the modes", so it refuses the
call instead of storing a number it would read back as `0644`.

## `fsck` and `mkfs`

`fsck` reads the volume and reports. Without a flag it never repairs, and
that is the only form MicroFS has.

```text
microos> fsck
fsck: 0 file(s), 3 directory(ies), 484 of 487 block(s) free
fsck: the volume is consistent
```

A volume with a problem prints the count and the first problem and exits with
an error. `fsck: no volume is mounted` means what it says: there is no MicroFS
at `/` on this machine right now.

The same command checks a FAT volume, by the point it is mounted at or by the
name of the device; a device that is not mounted is mounted for the length of
the walk and forgotten again:

```text
microos> fsck fat /mnt/sd
fsck: FAT32, 6 file(s), 2 directory(ies), 1974 of 1984 cluster(s) free
fsck: the volume is consistent
```

The boot sector and the BPB, both copies of the table, the chains (a loop, a
cross-link, a cluster outside the volume), the entries of the root and of the
subdirectories, the checksum of the long names, the lost clusters and the
free count are read; nothing is written. A volume with more than 65536
clusters is refused with a message, because the map of the claimed clusters
of the checker would not cover it.

With `-y` (or `--repair`) the FAT volume is repaired and then checked again:

```text
microos> fsck fat /mnt/sd -y
fsck: FAT32, 6 file(s), 2 directory(ies), 1975 of 1984 cluster(s) free
fsck: 1 lost cluster(s) found, 1 freed
fsck: 1 repair(s) applied
fsck: 0 problem(s) after the repair
fsck: the volume is consistent
```

The lost clusters go back to the free map, a loop or a cluster outside the
volume is cut, the second of two names of one cluster gives it up, a broken
long name is removed and a size that does not match its chain is put right.

An ext2 volume is checked the same way, by its point, and `-y` puts the
counters, the two bitmaps, the link counts and the chain of the directories
right and reconnects a record no name reaches under `/lost+found`
([ADR 0029](../adr/0029-ext2-write.md); `make x86-fs-smoke` compares the
repair with the `e2fsck` of the host):

```text
microos> fsck ext2 /
fsck: ext2, 12 file(s), 4 directory(ies), 12184 of 12288 block(s) free
fsck: the volume is consistent
```
The boot sector, two copies of the table that differ and a directory whose
head another chain holds are left and printed as `(not repaired)`; the wiki
of the files has the rules ([`files.md`](files.md#mkfs-and-fsck)).

`mkfs -y` unmounts the volume, formats it and mounts it again. The `-y` is
mandatory: everything in MicroFS is destroyed. The files of `/etc` come back
from the store of the port at the next boot (or at `config sync`), because the
configuration keeps a copy outside the filesystem; the rest does not come
back. On the board `idf.py erase-flash` destroys the store as well.

## `httpd`: the page of the state

`httpd` opens a small HTTP server on the machine. It is not started by the
boot: a machine of this size opens a port when its owner says so, from the
shell or from a script of `/etc/init.d`.

```text
microos> httpd
httpd: the page is on port 80
microos> httpd 8080
httpd: already on port 80
microos> httpd off
httpd: stopped
```

| Request | Answer |
|---|---|
| `GET /` | the name, version, architecture, uptime, memory, PSRAM and task count |
| `GET /health` | `ok` (a page of plain text, for a script) |
| any other path | `404 Not Found` |
| a method other than `GET` | `405 Method Not Allowed` |

Moving the server is `httpd off` and then `httpd <port>`: a second `httpd` with
a port only says where the page already is. A port that is not ours is
reported and the server stays off.

## `mqtt`: one message

The MQTT client is 3.1.1 at QoS 0 and 1. The shell command publishes **one**
message and disconnects; it is a way to send a word to a broker from a script
or from the console, not a client that lives in the board.

```text
microos> mqtt 10.10.10.1 1883 home/board hello
mqtt: 5 byte(s) to home/board on 10.10.10.1:1883
```

The limits are the ones of
[`mqtt.h`](../../components/net/include/net/mqtt.h): a topic of 128 bytes, a
payload of 256, a client id of 32, a session timeout of five seconds. There is
no TLS: the broker has to be on the network of the owner. A refused client and
a broker that does not answer are different sentences, so the console says
which one happened.

## `telnet` and `config`

`telnet` puts the shell on port 23 beside the UART and the panel, behind the
accounts of `/etc`: the client is asked for a name and a password (three
attempts, a wait and one sentence for a wrong password and for a name that
does not exist), the session runs as that user, `logout` ends it, and the door
never reads the UART of the machine. The server offers the echo (RFC 857) and
repeats the bytes when the client accepts, so a password is silent on both
sides; `telnet off` closes the door. The client sees the answers of its own
session and not the console of the board (ADR 0044).

`config` lists the store of `/etc` (the key-value store of the port, NVS on the
board), and `config sync` restores the files that are missing and saves the
ones that are there. The file wins when the two disagree — see [files.md](files.md).

A machine without a store says so instead of printing an empty list, and
`config sync` is an error there: there is no copy to restore from or save
into, and the files of `/etc` are the settings.

```text
microos> config
config: this machine has no store; the files of /etc are the settings
```

## The commands of a script

Every file of `/etc/init.d` is a script of the init process: one shell command
per line, `#` starts a comment, and each script runs in a task of its own. This
is how a service of the board starts without a person at the console:

```text
microos> mkdir /etc/init.d
microos> write /etc/init.d/clock date
microos> reboot
```

The commands of the script are the commands of this page, so a script can only
use what the machine registers. A line that fails is reported in the log and
does not stop the other lines of the script.

[Back to the wiki index](README.md)
