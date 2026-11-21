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

The line is split on spaces and tabs; there are no quotes, no escapes and no
globs. A line is at most 127 characters and at most 8 words per stage.
Backspace erases, Ctrl+C throws the line away, and a line that did not fit is
**not** executed (the shell says `line too long, it was not executed`),
because a cut line is a different command. While a program or a pipe runs,
Ctrl+C is not a line: the shell reads it and sends `SIGINT` to the program or
to every stage of the chain (see [kernel-abi.md](kernel-abi.md#signals)).

Because there are no quotes, a command that needs an empty argument accepts
the two characters `""` as the way to type it. `path ""` is the example.

A name that is not registered answers `<name>: command not found`. A command
that is given the wrong arguments answers with its own `usage:` line and does
nothing.

## Pipes and redirections

`|` joins two or more stages, and `>`, `>>`, `<`, `2>` and `2>>` send a
descriptor of a stage to a file. The operators are words of their own, with a
space around them, because the splitter has no quotes; they may stand before
or after the arguments of a stage:

```text
microos> ls /etc | cat
microos> echo one | cat | cat
microos> cat /etc/setting > /tmp/f
microos> cat /etc/setting >> /tmp/f
microos> cat < /etc/setting
microos> cat /etc/setting 2> /tmp/errors
microos> /bin/cat /etc/setting | wc -l
```

A stage that is a command of the shell (`ls`, `cat`, `echo`, `df`, ...) runs
in a task of its own and writes descriptor 1, so a redirect or a pipe carries
its output exactly as it carries a program's. A name that is not a command is
looked up in `PATH` and started with `execve`; on x86-64 that program
inherits the three descriptors of the stage, which is what makes
`echo one | /bin/cat` and `ls /bin | wc -l` work. On the board and in the
simulator there is no program to run, and only the commands of the shell can
be stages.

What the line does **not** do:

* no `&&`, `||`, `;` or `&`: they are refused by name, not run as arguments;
* no `2>&1`, no `>&`, no file-descriptor juggling beyond `0`, `1` and `2`;
* no quotes, escapes, globs or variables: `*` is a word and not a pattern;
* no attached forms: `a|b` and `>file` are arguments; write `a | b`, `> file`;
* at most three stages, because a boundary is a pipe and the shell task has
  four descriptors;
* no background job, no `jobs`, no `fg` or `bg`;
* a command that changes the task of the shell (`cd`, `path`) cannot be a
  stage or carry a redirection: it is refused with a sentence instead of
  changing a copy nobody sees.

The status of a line is the status of its last stage. A program that is the
last stage answers 0 when it ran and 1 when it could not start; its own code
is not handed back by `execve` yet.

## `help`

```text
microos> help
command   what it does
mkfs      format the volume: mkfs -y
...
microos> help exec
exec: run a program: exec <program> [args...]
```

`help` with an argument prints the same text as `<command> --help`: the usage
line, the summary and the notes. `help --help` explains `help` itself.

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
| `uptime` | the time since the boot, the ticks and the task count |
| `ps` | the tasks: pid, priority, state, name |
| `free` | the memory of the heap and of the PSRAM |
| `dmesg` | the log of the boot, from the ring buffer of the kernel |
| `echo <words>` | print the words |
| `sleep <milliseconds>` | wait, up to 60000 |
| `clear` | clear the screen (the escape sequence the panel and the VGA read) |
| `modules` | run the hardware doctor and print its report |
| `abi` | the table of the system calls |
| `memtest [kilobytes]` | measure the memory and its speed |
| `mfetch [color]` | the system report with a logo |
| `reboot` | restart the machine |

The files, on every machine:

| Command | What it does |
|---|---|
| `pwd`, `cd <dir>` | the current directory |
| `ls [-l] [-a] [path]` | the tree; `-l` adds mode, owner and size, `-a` the hidden `.` and `..` |
| `cat [-n] [file]` | the content of a file, or of the standard input; `-n` numbers the lines |
| `write <file> <text>` | create or replace a file with one line (at most 127 bytes) |
| `mkdir <dir>`, `rm [-r] <file>` | a directory, a file; a directory needs `-r` |
| `df [-h] [path]` | the free blocks and inodes; `-h` in KB/MB |
| `mount` | the mounted filesystems |
| `fsck [fat <point\|device>]` | check a volume, read-only: MicroFS, or the FAT volume at a point or on a device |
| `stat <path>` | the type, the mode, the owner and the size |
| `chmod <mode> <path>`, `chown <uid> <path>` | the mode (octal) and the owner (0..32767) |
| `path [directories]` | show or set `PATH` |
| `exec <program> [args...]` | run a program, by a path or found in `PATH` |
| `kill [-signal] <pid>` | send `INT`, `TERM`, `CHLD` or `PIPE` to a task; TERM by default |

The services, where the port registers them:

| Command | What it does | Where |
|---|---|---|
| `config`, `config sync` | list the store of `/etc`, restore and save it | board, simulator |
| `mkfs -y` | format the volume (erases everything) | board, simulator |
| `http get <url>` | one HTTP request; prints the status and the body | board |
| `ntp [server]` | set the clock from NTP (UTC), default `pool.ntp.org` | board |
| `httpd [port\|off]` | the page of the state over HTTP, default port 80 | board |
| `mqtt <host> <port> <topic> <message>` | publish one message at QoS 0 | board |
| `telnet [on\|off]` | the shell on port 23 | board |
| `date`, `date set <YYYY-MM-DD> <HH:MM:SS>` | the RTC, or the clock of the host | board, simulator |
| `battery`, `beep [frequency] [ms]`, `led on\|off\|heart\|dance` | the power cell, the buzzer, the LED | board, simulator (stubs) |
| `input [seconds]` | watch the buttons | board, simulator (stubs) |
| `wifi [status\|scan\|connect <ssid> [key]\|join\|disconnect\|forget]` | the WiFi station | board, simulator (stub) |
| `display [<x> <y>] \| rotate <0\|90\|180\|270> \| invert [on\|off] \| draw [seconds]` | the panel | board |
| `i2c` | scan the I2C bus | board |
| `mic` | the level of the microphone | board |
| `rtc alarm [HH:MM\|+<seconds>\|day <1-31> HH:MM\|clear]` | the alarm of the BM8563 and its flag | board |
| `power [sleep <seconds>\|off]` | deep sleep with the timer or button C, or power off | board |
| `factory reset -y` | erase MicroFS and the NVS, then restart; without `-y` it only explains | board |

The simulator has no network and no drivers: `http`, `ntp`, `httpd`, `mqtt`
and `telnet` are not registered there at all, and the driver commands of the
board are stubs that answer something sensible. The x86-64 port registers only
the core and the files: no `config`, no `mkfs`, no `date`, no network. This is
the port boundary doing its job, not a bug; [ports.md](ports.md) has the whole
matrix.

## `PATH` and `exec`

`PATH` starts at `/bin:/usr/bin` and belongs to the shell: it is not a file of
`/etc`, and a program does not inherit it yet.

```text
microos> path
/bin:/usr/bin
microos> path /bin
microos> path ""
```

A name with a slash is a path and is used as it is. A name without one is
looked up in `PATH`, and the first regular file with an execute bit wins; a
file without the bit is passed over, as in Linux. A search that found nothing
says where it looked:

```text
microos> exec hello
exec: hello: not found in /bin:/usr/bin
```

On the board and in the simulator there is no program to run: a fresh volume
has no `/bin`, and `execve` answers `ENOSYS` even for a file that is there. The
x86-64 port writes `/bin/hello`, `/bin/crash`, `/bin/fork`, `/bin/badptr` and
`/bin/signals` at the boot, and runs them as programs of ring 3. It also
writes the library programs `cat`, `echo`, `ls` and `wc`, whose descriptors
are inherited from the shell, so a pipeline can end in one of them.
[programs.md](programs.md) explains what a program is.

## `kill`, Ctrl+C and no job control

There is no job control: no `&`, no `fg`, no `bg`, no groups of processes and
no `SIGSTOP`. A program runs in the foreground until it ends, and the shell
waits for it. What a person has instead is two ways to stop one:

```text
microos> ps
  pid  prio  state     name
   12     5  running   shell
   13     5  sleeping  program
microos> kill 13
microos> kill -INT 13
```

`kill` takes a positive pid from `ps` and sends `TERM` unless a signal is
named: `-INT`, `-TERM`, `-CHLD`, `-PIPE` or the number. A pid that names no
task answers `kill: 42: no such process`; a signal outside the four is
refused. During an `exec`, Ctrl+C of the console sends `SIGINT` to the
program; during a pipeline it goes to every stage and the programs they
started, because they share one job (ADR 0017). A program that ignores it
keeps running: there is no `-9` to fall back on (a number outside the table
is `EINVAL`), and the machine is restarted with `reboot` if a program really
hangs.

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

`fsck` reads the volume and reports; it never repairs.

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
of the checker would not cover it. Repair is not in this kernel.

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

`telnet` puts the shell on port 23 beside the UART and the panel; the
negotiation is answered with refusals and the client keeps its own echo. There
is no authentication, and `telnet off` closes the door.

`config` lists the store of `/etc` (the key-value store of the port, NVS on the
board), and `config sync` restores the files that are missing and saves the
ones that are there. The file wins when the two disagree — see [files.md](files.md).

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
