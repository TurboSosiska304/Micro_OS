# Micro OS documentation

Micro OS is a Linux-like **kernel** for small machines: one portable core,
and a port per kind of machine. The board (M5Stack StickC Plus2) is one of
those ports, not the project. This folder documents the kernel; the quirks of
the board live in `hardware/`.

## Where to start

| If you want to... | Read |
|---|---|
| know what the system is | [../README.md](../README.md) |
| contribute, build, run the checks | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| understand the design | [architecture.md](architecture.md) |
| know what is done and what is next | [roadmap.md](roadmap.md) (the kernel) and [ports/esp32/roadmap.md](ports/esp32/roadmap.md) (the board) |
| bring the kernel to another machine | [porting.md](porting.md) |
| see the system call contract | [syscall-abi.md](syscall-abi.md) |
| see the memory model | [memory.md](memory.md) |
| see the libc subset | [libc.md](libc.md) |
| read why a decision was made | [adr/](adr/) |
| read what a phase delivered | [phases/](phases/) |
| know the board: pins, chips, quirks | [ports/esp32/hardware.md](ports/esp32/hardware.md) |
| review the portability work | [review.md](review.md) |

## Language

English is canonical: the documents here are written in it and the code
comments are in it. The Russian translations live in [ru/](ru/README.md) and
may lag behind - when they disagree with a document here, this one is right.

## The shape of the tree

* `adr/` - architecture decision records, one file per decision;
* `phases/` - the report of each phase: what it delivered and what it cost;
* `ports/` - one directory per port; the ESP32 port has its plan and the
  notes of the board there;
* `ru/` - the Russian translations of the documents above.
