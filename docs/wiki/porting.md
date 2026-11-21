# Porting

A **port** is a directory that implements the contract of
`components/kernel/include/kernel/port/*.h` and `kernel/port_task.h`; a
**board** is the description of a machine plus the drivers of its chips. The
kernel includes nothing of a platform, and `make integrity` fails when it
tries ([ADR 0004](../adr/0004-portable-boundary.md)). The same sources build
as a Linux process, as a bare-metal x86-64 machine and as the firmware of the
board because of that line.

The canonical guide, with the version of the contract, every group of calls,
the stubs of an absent duty and the common mistakes, is
[`docs/porting.md`](../porting.md). This page is the short way in; the guide
and the headers are the truth.

## The shape of a port

| Group | Header | What the machine answers |
|---|---|---|
| execution | `kernel/port_task.h` | stack, tasks, sleep, signals |
| console | `kernel/port/console.h` | the bytes out, a line, a key, a poll |
| clock | `kernel/port/time.h` | milliseconds and microseconds since the boot |
| heap | `kernel/port/mem.h` | the internal heap, the PSRAM, alloc and free |
| locks | `kernel/port/lock.h` | a critical section and a recursive mutex |
| power | `kernel/port/power.h` | halt and reset |
| identity | `kernel/port/info.h` | the name, the architecture, contract 1.8 |
| store | `kernel/port/kv.h` | the keys behind `/etc`, or "nothing is stored" |
| network | `kernel/port/net.h` | TCP client and server, one UDP exchange |
| programs | `kernel/port/exec.h` | a window, the entry, the end, a pointer check |

The first seven are mandatory; the store, the network and programs may be
stubs that answer honestly. The execution substrate is never optional: the
scheduler has no context to switch without it.

Two duties have no `k_port_*` call: the boot (`kernel_boot()` with the board
description and the subsystem table, `kernel/boot.h`) and the block devices
(`struct k_blockdev` registered with `k_blockdev_register()`,
`kernel/blockdev.h`). Drivers are not port duties.

## The order that hurts least

1. console — one character out, everything else is debugged through it;
2. time — a counter that only goes forward;
3. execution substrate — the kernel's own tests then run;
4. memory — `kmm` and the VFS start working;
5. locks — the critical section first, the sleeping mutex after;
6. block device — MicroFS can mount;
7. store, network, programs — each may say "I have none";
8. board and drivers.

Read the existing ports in this order: `ports/port_null` (the shape with no
machine), `ports/port_host` (the same calls over POSIX), `ports/port_x86_64`
(the bare machine, programs, signals and an MMU) and
`components/port_esp32` (the board).

## What checks it

```bash
make test            # the kernel against port_host, and port_null linked
make integrity       # [layers], [port], [budget], [sources], [format]
make x86-smoke       # a second machine boots
tools/device-test.sh /dev/ttyACM0   # the board boots
python3 tools/wiki_check.py
```

`[port]` looks for every function of the contract in every port directory: the
built library for the host and null ports, the sources for the ESP32 and
x86-64. A boot says `port: <name>, contract 1.8`; a major mismatch panics and
a minor one warns.

## What a port does not do

It does not add a call to the contract for one board, include a driver header
or know the syscall ABI. If the kernel needs a new door, that is a new call
and a bump of the contract for every port — that is what the version is for.

[Back to the wiki index](README.md)
