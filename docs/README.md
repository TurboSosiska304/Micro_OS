# Micro OS documentation

Micro OS is a Linux-like **kernel** for small machines: one portable core,
and a port per kind of machine. The board (M5Stack StickC Plus2) is one of
those ports, not the project. This folder documents the kernel; the quirks of
the board live in `hardware/`.

## Where to start

| If you want to... | Read |
|---|---|
| know what the system is | [../README.md](../README.md) |
| use the system, not read about it | [wiki/](wiki/README.md) (build, shell, files, programs, ports, troubleshooting) |
| contribute, build, run the checks | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| understand the design | [architecture.md](architecture.md) |
| know what is done and what is next | [roadmap.md](roadmap.md) (the kernel) and [ports/esp32/roadmap.md](ports/esp32/roadmap.md) (the board) |
| bring the kernel to another machine | [porting.md](porting.md) |
| port a program (zlib, PCRE2, Lua, sbase) to the userland | [porting-programs.md](porting-programs.md) |
| move a machine from 0.x to 1.0 | [migration.md](migration.md) |
| know what a release changed | [../CHANGELOG.md](../CHANGELOG.md) |
| report a vulnerability | [../SECURITY.md](../SECURITY.md) |
| see the system call contract | [syscall-abi.md](syscall-abi.md) |
| see the memory model | [memory.md](memory.md) |
| see the libc subset | [libc.md](libc.md) |
| read why a decision was made | [adr/](adr/) |
| read what a phase delivered | [phases/](phases/) |
| know the license of a byte of a distribution | [licensing.md](licensing.md) |
| read about a port | [ports/esp32](ports/esp32/README.md), [ports/x86_64](ports/x86_64/README.md), [ports/host](ports/host/README.md), [ports/null](ports/null/README.md) |
| know the board: pins, chips, quirks | [ports/esp32/hardware.md](ports/esp32/hardware.md) |
| review the portability work | [review.md](review.md) |

## Language

English is canonical: the documents here are written in it and the code
comments are in it. The Russian translations live in [ru/](ru/README.md) and
may lag behind - when they disagree with a document here, this one is right.

## The shape of the tree

* `adr/` - architecture decision records, one file per decision;
* `phases/` - the report of each phase: what it delivered and what it cost;
* `ports/` - one directory per port, with its notes and its plan: `esp32`
  (the board), `x86_64` (bare metal in QEMU), `host` (the tests and the
  simulator), `null` (no machine: the proof of the boundary);
* `ru/` - the Russian translations of the documents above.
