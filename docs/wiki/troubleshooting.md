# Troubleshooting

The table is ordered by where the trouble shows up: the build, the x86-64
machine, the board, the shell, the files, the network commands, the checks and
the tools. A row names the sentence you see, the cause and the move.

## The host build

| Symptom | Cause | What to do |
|---|---|---|
| `CMake Error ... No such file` after a move | a build tree with the old paths | `make clean`, then `make test` |
| `ninja: command not found` | ninja is not installed | `pacman -S ninja` / `apt install ninja-build` |
| `clang-format not found` | the formatter is a package of its own | install `clang-format`; the check is skipped without it |
| the tests fail, the same commit is green in CI | a stale `build-host/` | `make clean`, then `make test` |
| `make sim` waits and prints nothing | the simulator is interactive | type a command; end stdin with Ctrl+D; `make smoke` is scripted |
| the simulator formats its volume every boot | the volume path is missing or not writable | use a directory that exists, or `/tmp/microos-sim.img` |
| `tsan: ... data race` | a race the host port cannot hide | a defect of the kernel or the test, never noise |

## The x86-64 machine

| Symptom | Cause | What to do |
|---|---|---|
| `QEMU has no 'gtk' display` | QEMU was built without GTK | `pacman -S qemu-ui-gtk` / `apt install qemu-system-gui` |
| ... and no package is wanted | a machine without a display server | `make x86-run X86_DISPLAY=sdl`, or `make x86-run-serial` |
| `qemu-system-x86_64: command not found` | QEMU is not installed | `pacman -S qemu-system-x86` / `apt install qemu-system-x86` |
| `x86-smoke is skipped` in `make ci` | QEMU is not on `PATH` | install it; the rest of `make ci` is valid without it |
| the window opens, typing does nothing | the keys go to the window's console | type into the window, or use `make x86-run-serial` |
| `/bin/hello` is not found in the smoke test | a stale image | `make x86-build` again; `rm -rf build-x86` if in doubt |
| `build-x86/disk.img` is missing | the image is made on demand | `make x86-run-disk`, or `x86_disk_smoke.py --mkimage` |
| the disk keeps files from an old run | the image is persistent by design | remove `build-x86/disk.img`; the next boot formats it |

## The board

| Symptom | Cause | What to do |
|---|---|---|
| `ESP-IDF not found: set IDF_PATH or IDF_EXPORT` | no `export.sh` was found | set `IDF_EXPORT`, or use `../esp-idf` or `~/esp/esp-idf` |
| `export.sh`: "no installed versions" | the registry does not know this checkout | run `esp-idf/install.sh esp32` once |
| `/dev/ttyACM0: Permission denied` | the port belongs to `uucp` | `gpasswd -a $USER uucp`, then log out and back in |
| the wrong board is flashed | several ports are connected | `make idf-flash IDF_PORT=/dev/ttyACM0` |
| `device-test.sh` never prints `RESULT: PASS` | a short console, or the port is busy | `CONSOLE_SECONDS=60`, close `idf.py monitor` first |
| nothing on the serial console | a charge-only cable, or no reset | use a data cable, press reset, try `console.py --reset` |
| the volume is gone after a flash | `idf.py erase-flash` wipes MicroFS | expected; restore `/etc` from the store, the rest is lost |
| `/etc` is empty after a format | the store was erased too | `mkfs -y` keeps the store; `erase-flash` does not |

## The shell

| Symptom | Cause | What to do |
|---|---|---|
| `<name>: command not found` | the command is not of this port | see [shell.md](shell.md); `httpd`, `mqtt`, `telnet` are the board's |
| `line too long, it was not executed` | the line is over 127 characters | shorten it; a cut line is refused on purpose |
| the last words are ignored | the line is cut at 8 words | split the command |
| `usage: ...` and nothing happens | the wrong number of arguments | the line says what it wants; `help <command>` repeats it |
| `help: no such command` | the name is not registered | `help` alone lists them |
| `-h` / `--help` prints nothing | the flag is not the first word of the line | put it first, or use `help <command>`; `df -h` is sizes, so `df --help` |

## Files and `/etc`

| Symptom | Cause | What to do |
|---|---|---|
| `fsck: no volume is mounted` | `/` is not MicroFS right now | `mkfs -y` formats and mounts; check `MICROOS_VOLUME` |
| `fsck` reports problems | a volume written by a crash | it only reports; copy what you can, then `mkfs -y` |
| `fsck fat /mnt/sd` reports problems | a FAT volume a crash left inconsistent | copy what you can first, then `fsck fat /mnt/sd -y` repairs the safe damage; the boot sector, two table copies that differ and a directory whose head is shared stay `(not repaired)` for a person |
| `stat` prints `mode: (not set)` | `/proc`, `/dev`, or an old volume | expected: those records keep no modes |
| `chmod 000` is refused | zero means "the modes predate the record" | choose another mode; a real zero needs a format change |
| "not permitted" for a file you own | uid 0 gets no exemption of root | `chmod` the triad you need, for example `0644` |
| `rm` of a directory fails | `rm` removes files, there is no `-r` | remove the children first |
| a file of `/etc` returns after `mkfs -y` | the store restored it | expected; change it, then `config sync` |
| writes vanish after an x86-64 reset | the boot ran on the image of the build (`make x86-run`), or no disk was attached (`ram0`) | `make x86-run-disk` keeps `build-x86/disk.img` between runs |

## Programs

| Symptom | Cause | What to do |
|---|---|---|
| `exec: hello: not found in /bin:/usr/bin` | there is no `/bin` here | x86-64 has `/bin` in the disk image: `make x86-disk` builds it; `path` shows the search list |
| `this machine cannot run a program` | `execve` is `ENOSYS` on this port | expected on the board and in the simulator |
| `not permitted: the file has no execute bit` | the mode has no `x` for the caller | `chmod 0755 <path>` |
| `not a MOX1 image of this system` | the file is not an image, or the header is wrong | build it with `mkprog.sh`, for the window of the port |
| `not enough memory for the image` | the image does not fit the window | a smaller image; the window is 16 MiB at 32 MiB |
| `the program faulted: vector N at 0x...` | the program touched memory not its own | the report of ring 3, not a crash; the shell survives |
| a program gets 14 from a call | `EFAULT`: the pointer is not its memory | fix the pointer; the null and guard pages are unmapped |

## Network commands

| Symptom | Cause | What to do |
|---|---|---|
| `httpd: the port 80 is not ours` | the port is held, or the stack refused | try `httpd 8080`; check `wifi status` |
| `httpd: already on port 80` | the server is on | `httpd off`, then `httpd <port>` |
| `http: <url>: the connection failed` | no route, no DNS, or the server is down | `wifi status`; try an address; there is no TLS |
| `the URL is not one we speak` | the scheme is not plain HTTP | only `http://` with a host is understood |
| `ntp: no answer from ...` | UDP 123 is blocked, or the server is silent | try `ntp time.google.com` |
| `mqtt: no session with the broker` | the broker is unreachable on that port | check host and port; there is no TLS |
| `mqtt: the broker refused the client` | the broker said no | the client id is `microos`; check its rules |
| `mqtt: the message is longer than 256 byte(s)` | the payload limit | shorten it; a topic is 128 bytes at most |
| `telnet` connects but nothing happens | the door is off | `telnet on`; the port is 23, no authentication |

## The checks and the tools

| Symptom | Cause | What to do |
|---|---|---|
| `make integrity` fails on formatting | the sources are not formatted | `make format`, then run it again |
| it fails on whitespace, a tab or 100 columns | the rules of `.editorconfig` | the check names the file and the line |
| it fails in `[layers]` or `[port]` | a platform header in the kernel, or a gap in a port | the failure names the symbol; see [ADR 0004](../adr/0004-portable-boundary.md) |
| it complains about `dist/` | a bundle of another version | `make bundle`, or remove `dist/` |
| clangd or Serena shows no symbols | no firmware build, so no `compile_commands.json` | `make idf-build` once |
| `console.py` fails on `serial` | `pyserial` is not in that Python | install it, or use the ESP-IDF Python |

## When nothing here fits

The register of what the tree knows it does not do is
[`docs/debts.md`](../debts.md); the plan is [`docs/roadmap.md`](../roadmap.md).
For a build that fails in a way this page does not name, [`build.md`](build.md)
has the commands to repeat by hand, and [`testing.md`](testing.md) has the check
of the layer that broke.

[Back to the wiki index](README.md)
