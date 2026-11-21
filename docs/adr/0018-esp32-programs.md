# ADR 0018 - Programs on the ESP32, and what the port gets after 1.0

* Status: accepted
* Date: 2026-11-21

## Context

`execve` and the MOX1 format are in the kernel (ADR 0005, 0006, 0016), and one
machine runs them: x86-64 in QEMU, with ring 3, page tables per program and a
guard page. The board answers `ENOSYS` by contract, and the wiki says so.

The product needs a program on the board before 1.0: a `.mox` that a person
writes, builds and runs on the StickC Plus2. The machine makes that a different
problem from the one x86-64 solves:

* the ESP32 (Xtensa LX6) has **no MMU**: there is no address space per program
  and no page that can be made read-only, executable or absent;
* the flash is **executable through the instruction cache** when a region is
  mapped as instructions (`esp_partition_mmap` with `ESP_PARTITION_MMAP_INST`),
  but the mapping starts on a 64 KiB boundary;
* the internal SRAM is about 100 KB for the kernel, the drivers and the
  stacks; the 2 MB of PSRAM are data, not code;
* the x86-64 code of an image is not Xtensa code: the same file cannot run on
  both machines.

The port is one of several, and after 1.0 it is a compatibility port, not the
place where the system grows.

## Decision

### 1. MOX1 version 2 carries the machine

The field `reserved` of the header (offset 28) becomes `arch`: `0` is x86-64,
`1` is Xtensa (`K_EXEC_ARCH_X86_64`/`K_EXEC_ARCH_XTENSA` in `kernel/exec.h`).
The version of the header is 2. The parser carries the machine of an image and
refuses a number that is neither 0 nor 1 with `EINVAL`; whether an image of
another machine may run is not a fact about the file, so it is the question of
`k_exec_run()`: it compares `image->arch` with the new call of the port,
`k_port_exec_arch()` (0 x86-64, 1 Xtensa, -1 for a port that runs no programs),
and answers `K_EXEC_RUN_OTHER_ARCH`, which `execve` turns into `ENOEXEC`,
**before a byte of code runs**. A port that cannot run programs still names
the machine it is, so the wrong file is `ENOEXEC` and only the right file of a
machine without a loader is `ENOSYS`. The version is part of the answer the
same way: an image of version 1 (or of a later generation) is refused, and the
kernel log says which version the kernel reads.

The port contract moves from 1.7 to 1.8, which is that one call.

`tools/mkprog.sh` takes `--arch x86-64|xtensa` and writes the number (x86-64
is the default); the build of a program names the machine it is built for.

### 2. The programs live in flash of their own

A `progs` partition (data, subtype `0x41`, 256 KiB) is carved from the end of
the present MicroFS region. A program is a `.mox` stored at an offset that is a
multiple of 64 KiB inside that partition, so the region can be mapped as
instructions; `mkprog` pads an image to that boundary when it is asked to, and
the loader records the offset in the file (a small directory of name, offset
and length is enough for 1.0).

The layout of the flash is revisited with the safe update of 1.0 (A/B slots);
this ADR fixes the principle, not the final table.

### 3. The program is built for Xtensa and entered windowed

The same C and the same surface of the library, built with the Xtensa GCC of
ESP-IDF with the **windowed ABI** of the machine (the default, the one of
ESP-IDF itself), a linker script of the port and the same `_start`:

* text stays in the mapped flash and runs there;
* data is copied to internal RAM (or PSRAM) and bss is cleared by the loader;
* the stack is 8 KiB of the memory of the program;
* a system call is a **trampoline of the port**, not an instruction: the loader
  passes the program a pointer to a function with the convention of
  `ksys_call(number, a0 ... a5)`, the library of the program keeps it and every
  wrapper calls it. The numbers of `kernel/ksys.h` are the same on both
  machines;
* the entry receives the pointer of the door, `argc`, `argv` and `envp` as its
  four arguments;
* `exit` ends the task of the program and the `execve` of the shell answers
  with the status.

`call0` (the ABI without register windows) was measured and is not chosen: a
`call0` image can call a kernel function, but a deep kernel call made through
it (a handler of a console line, for one) breaks the window bookkeeping and
panics on the return. The windowed ABI is what the rest of the machine uses and
what a C compiler of this port expects.

### 3a. The `syscall` instruction is not the door

The vector of ESP-IDF takes `EXCCAUSE_SYSCALL` before the table of exception
handlers of the port (`components/xtensa/xtensa_vectors.S`), so a program that
executes `syscall` skips the instruction: the spike measured `-1` and the
number never arrived. Taking the vector over is a change of the substrate, not
of this format; the trampoline of the pointer is the door. A trap-based door
can be a later decision with its own price.

### 4. There is no protection, and that is the crutch

A program on the board is a task of the kernel with the kernel's own address
space. A bad pointer, a write to the text or a jump into a wrong place is a
fault of the machine: the exception handler of the port ends the task where it
can tell the program's text from the kernel's, and panics where it cannot. The
documentation, the wiki and the release notes say this in those words: a
program of the board can take the machine down, and a program of x86-64 cannot.

`k_port_exec_supported()` becomes true on the board, `execve` runs the image and
waits for it, and `tools/device-test.sh` runs `/bin/hello` on the board. The
simulator and the null port stay `ENOSYS`.

### 5. The format after version 2 is not promised to the board

Version 2 of the format is the one the ESP32 receives. A version 3 and later
are the format of the machines that can grow; the board is not promised a
reader for them, and a kernel of the board that meets one says `ENOEXEC`
rather than guessing. A compatibility port keeps the format it was frozen
with, and a program for the board is built and tagged against that version.

### 6. After 1.0 the port is a compatibility port

From `1.0.0` the ESP32 port:

* keeps building (`make idf-build`) and passing `tools/device-test.sh` on the
  frozen set of features;
* receives the features that are cheap on the board: memory handling, the
  parts of concurrency that FreeRTOS tasks give for free, driver fixes, a
  better console;
* does not receive the features that need an MMU, a larger toolchain or a
  rewrite of the port: a compiler of our own, the higher half, NX, full SMP,
  an address space per program.

A feature that is cheap is one an evening of work buys; one that is not is a
project of its own. The boundary is decided per feature and recorded in the
port roadmap, not promised in advance.

## What the spike measured

A `feature/esp32-exec-spike` branch (not merged) ran code in the flash of the
board:

* 256 bytes at `+0x1E0000` of `factory`, erased, written, mapped as
  instructions at `0x40170000` and called - it returned 42, both as a windowed
  function and as a `call0` entry;
* the payload called a kernel function through a pointer, printed a line with
  `klog` and returned; the windowed form worked, the `call0` form panicked
  after the callback returned (see 3);
* the `syscall` instruction came back as `-1` (see 3a);
* the cache did not serve a stale byte: after the mapping, a `memcmp` through
  the instruction mapping matched the written bytes, and the loader should keep
  that check;
* IDF refuses to erase or write the running application partition
  (`CHECK_WRITE_ADDRESS` aborts), which is one more reason the programs live in
  a partition of their own rather than in the tail of `factory`.

## Consequences

* One format, two machines: an image says which machine it is for, and the
  kernel says no to the wrong one instead of jumping into it.
* A program on the board is not isolated; the price is written down.
* The flash is repartitioned: MicroFS loses 256 KiB until the update of 1.0
  returns the space with the A/B layout.
* The `progs` partition and the 64 KiB alignment are a constraint on the
  format's use, not on the format itself.
* The promise of the format ends at version 2 on this port: a later version
  is a new contract and the board may be left behind by it.
* A bytecode VM is not chosen: it is slow on a 240 MHz core and it is a second
  toolchain. If the project writes a compiler of its own, it is a second
  `arch`, and the compatibility rule decides whether the board gets it.

## Alternatives

* **Copy the image to internal SRAM.** Executable, but the kernel and the
  drivers leave about 50 KB and the pool would be one program at a time.
* **A bytecode VM.** Slow, and it needs an interpreter and a compiler before
  the first program runs.
* **No programs on the board.** Rejected by the product: the board is the
  machine a person carries.
