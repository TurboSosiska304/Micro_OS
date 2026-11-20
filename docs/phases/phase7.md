# Phase 7 - Programs and the edge of the system

A program stops being a function of the tree. It becomes a file: a file in
`/bin` with a format the kernel parses, an owner and a mode - and the
arguments of a system call become something a stranger wrote. The decisions
are in [ADR 0005](../adr/0005-programs-and-protection.md).

Two machines decide what the phase can promise. The board has no MMU, and its
flash is not executable for a file the kernel did not link into the image, so
the board gets the permissions and an honest `ENOSYS`; x86-64 in QEMU has RAM
it can execute, and it is the machine the jump belongs to. The kernel stays
one tree: the format and the checks are portable, the jump is the port's.

The phase as it stands: `chmod`, `chown` and `execve` are numbers 23, 24 and
25 of the table, the mode of a file is checked at `open` and at `execve`, the
library has the three wrappers, the shell has the commands, and the x86-64
port writes a program to `/bin/hello` of a volume in its memory and asks for
it with `execve` at its boot. The end of this document says what that leaves
open.

This is the first implementation phase of roadmap 7; what is not here is
listed at its end and in [docs/roadmap.md](../roadmap.md).

## What is there

### The format of a program: MOX1

`kernel/exec.h` is the format, `components/kernel/src/exec.c` is the parser, and
`tools/mkprog.sh` turns an object into an image.

* **The header** is 32 bytes, eight words of 32 bits, little endian, read byte
  by byte and never through a struct: the bytes of a file have no alignment,
  and a cast to a word pointer is a fault on a machine that does not forgive
  it. The fields are the magic `MOX1` (the bytes; the little-endian word is
  `0x31584F4D`), the version (1), the flags (0), the offset of the entry, the
  size of the text, of the data and of the bss, and a reserved word.
* **The image is flat**: the header, then the code, then the initialised data.
  It is linked for the window of the port - the code at the base of the window
  plus the 32 bytes of the header, the data after it - because nothing
  relocates it, so it is copied as it is. The bss is not in the file: the port
  clears it before the entry.
* **The parse checks the image against itself** and nothing else:
  the magic and the version, a flag or a reserved word this generation does
  not know, sizes that are not multiples of four (the port copies in words, an
  odd size would leave a tear), a text of zero bytes, an end beyond the file,
  an entry in the header or in the data instead of the text, and a bss above a
  megabyte (`K_EXEC_BSS_MAX`): the zeroed part costs memory and is not in the
  file, so the parser is the only place that can put a ceiling on it.
* **The sum of the sizes is computed in 64 bits.** Two fields of `0xFFFFFFFC`
  pass a 32-bit comparison by wrapping; the test carries exactly that image.
* **`tools/mkprog.sh`** takes the object, `.text` as the text, `.data` and
  `.rodata` together as the data, the size of `.bss` as the bss, and writes the
  entry as an offset from the start of the image. It refuses an entry symbol
  that is not in `.text`, and it pads every segment to four bytes before the
  next one starts, so that the copy in words has no tear and the data lands
  where a program linked for the window reads it.

### `execve` is a duty of the port

The contract is 1.3 (`KERNEL_PORT_API_VERSION_MINOR`), and
`kernel/port/exec.h` is the ninth subject of it: three calls, and the closure
test of the contract counts them.

* `k_port_exec_supported()` - can this machine run a program from a file at
  all;
* `k_port_exec_base()` - the first address of the window of the programs;
* `k_port_exec_enter(image, argv)` - copy the image there, clear the bss, make
  `argv` the vector of the program, transfer control. A port that returns
  refused the image; a program that finishes the call is noticed by the port
  and the call ends with 0 (see below).

`k_exec_run()` sits between the parser and the port and answers in the words
of the syscall layer: `-2` when the machine cannot (the kernel turns it into
`ENOSYS`), `-1` when the image does not fit, `0` when the port entered a
program and the program came back. It asks whether the machine can before it
looks at memory, and it checks the entry and the bss again, because a
structure that did not come from the parser cannot be trusted; the window is
`base + text + data + bss`, with a refusal for the zero page and for a sum
that wrapped.

Who answers what:

* `port_esp32` - no. The flash is not executable for a file, and a copy in
  internal SRAM would live in the ~100 KB the kernel and the stacks share.
* `port_host` - no. The simulator is a process of the development machine, and
  jumping into the bytes of a program would run arbitrary code with the libc
  of the host under it; the format is exercised there as data.
* `port_null` - no. No memory to place an image in, no processor to jump to.
* `port_x86_64` - yes. The window starts at 32 MiB (`0x02000000`) and ends at
  48 MiB (`0x03000000`), the heap of the port stops below it, the kernel is
  linked at 16 MiB and the identity map of the loader covers the first
  gigabyte. A stack of 64 KB is cut from the top of the window, so an image
  has to end below it. At the boot the port also mounts MicroFS on a volume of
  its own RAM (`src/blockdev_ram.c`), so the program has a file to come from.

The answers are the point of the list: the kernel keeps a parser that is
tested on a machine that cannot run what it parses, and the machine that can
run a program says so in its own directory.

#### The jump of the x86-64 port

`k_port_exec_enter()` copies the header and the segments to the window,
clears the bss, and enters the program on a stack of its own with the
interrupts off. The state of the kernel is saved by hand: the callee-saved
registers are pushed, the stack pointer of the boot goes into a variable of
the file, and the address of a small exit stub is pushed onto the stack of the
program, so a `ret` at its end comes back to the port as if the jump had been
a call. A program that halts never reaches the stub, which is the shape a
program of the system has.

Today the program is `/bin/hello` of that port, and it is a file:

* `ports/port_x86_64/programs/hello.c` is compiled and linked at
  `0x02000020` - the base of the window plus the header - by
  `programs/program.ld`, turned into a MOX1 image by `tools/mkprog.sh`, and
  embedded in the kernel as a blob by the build of the port;
* at the boot the port mounts MicroFS on a volume of its RAM
  (`src/blockdev_ram.c`), writes the image to `/bin/hello` and asks for it
  with `ksys_execve()`; the call stats the file, checks its mode, reads it
  through the VFS, parses it and hands it to `k_exec_run()`;
* the program writes `PROGRAM: hello from /bin` to COM1 one byte at a time,
  with no libc and no kernel under it, and ends with a `ret` so that the boot
  continues into the shell. `make x86-smoke` checks that the port enters a
  program, that the program writes its line and that the kernel is back from
  it. The message lives in the data segment on purpose: a port that copied
  the text alone would print zeros there.

The round trip is the point of the milestone: the bytes that are executed have
been through a block device, MicroFS and the VFS, and the file is what the
kernel holds and not a variable with the image in it. The boot goes through
`sys_execve()`: the image is embedded in the kernel because the machine has
no filesystem before it mounts one, but the stat of the file, the mode, the
read through the VFS and the call are exactly the ones a program of `/bin`
takes.

### The loader: three calls in the table

`KSYS_CHMOD` is number 23, `KSYS_CHOWN` is 24 and `KSYS_EXECVE` is 25. The
numbers are frozen like the rest of the table: a new call takes the next free
one, and a number is never reused.

`chmod(path, mode)` and `chown(path, owner)` are the thin half:

* the path is checked at the edge, then the number: a mode outside
  `KSYS_MODE_MASK` (nine bits) and an owner outside `KSYS_UID_MAX` (fifteen
  bits) are `EINVAL`, so a number that does not fit the record can never spill
  into the type of the object;
* the two calls end in `vfs_chmod()` and `vfs_chown()`, which are operations a
  filesystem may have or not. MicroFS has them; `procfs` and `devfs` answer
  "not permitted" instead of storing a number that nothing would read.

`execve(path, argv)` lives in `sys_execve()` beside the other calls, because a
program starts as a file of the filesystem like any other object:

* the path is checked at the edge, then the object is described with
  `vfs_stat()`: a directory is `EISDIR`, an object that is not a file (a
  device of devfs) is `EPERM`, and a file whose mode has no execute bit for
  the caller is `EPERM` - a mode of zero (a filesystem without modes) is not a
  refusal;
* a file smaller than the header or larger than `KSYS_MAX_IO` is refused
  before a byte of it is read, so a wrong file of the volume cannot ask for
  the memory of the system;
* the image is read into one block of `kmm` through the VFS - not through a
  descriptor of the caller, so a failed `execve` leaves the table of the task
  exactly as it was - parsed by `k_exec_parse()`, and handed to
  `k_exec_run()`;
* the block is given back on every way out; a port that enters a program and
  never comes back takes the memory of the task with the program, and a
  program that returns ends the call with 0, because a machine without a
  process to exit into has to notice a program that finished.

A machine without a window for a program answers `ENOSYS` here, from
`k_exec_run()`, and that is the whole of `execve` on the board and in the
simulator.

`argv` is part of the signature and not of the implementation yet:
`sys_execve()` marks it unused, `k_exec_run()` takes only the image, and the
port is entered with a NULL vector. The ABI of the entry - the registers, the
stack, its alignment, where the strings live - is not written down, so the
kernel would have nothing to promise even if it walked the array.

### The library over the calls

`components/libc` carries the three wrappers, so a program of the system calls
them by their POSIX names (`chmod`, `chown`, `execve` are macros over
`microos_*` in `libc/unistd.h`):

* `microos_chmod()` and `microos_chown()` refuse NULL and a number wider than
  the record (`LIB_MODE_MASK`, `LIB_UID_MAX`) before the kernel is asked, set
  `errno` to the code of the refusal and return -1; a success clears `errno`
  and returns 0;
* `microos_execve()` passes the path and the vector to number 25. A call that
  enters a program does not return; a machine without a process to exit into
  comes back with 0 when the program has finished, and that zero is the answer
  of the library too - the caller is the program again and there is nothing to
  report. A refusal is the negative code in `errno`.

The numbers of `errno` are the numbers of POSIX, so a program reads `EPERM`,
`EINVAL`, `ENOENT` and `EISDIR` and not the codes of the kernel.

The shell reaches the same three calls: `chmod <mode> <path>`,
`chown <uid> <path>` and `exec <program> [arguments...]`. The commands parse
and resolve the argument (an octal mode, a decimal uid, a path relative to the
working directory) and print the reason the library gives; `exec` builds the
vector on the stack of the shell and does not read it back.

### The mode and the owner of a file

MicroFS keeps its 16-byte records: the mode (9 bits, `rwx` for the user, the
group and the others) and the owner (15 bits, a uid) are packed into the
upper bits of `flags`, the two low bits of which are the type. The layout and
the reasons are in [docs/memory.md](../memory.md).

* New files are `0644`, new directories `0755`.
* `mfs_chmod()` and `mfs_chown()` write the record and sync the volume;
  `mfs_inode_mode()` and `mfs_inode_owner()` are the only readers of the
  shifts, so the shell, the syscalls and the check of the access never know
  where the bits live.
* The checks are of the fields, not of the values: a mode outside nine bits
  or an owner outside fifteen is refused, so a number that does not fit can
  never spill into the type of the object.
* A listing carries the two numbers beside the name and the size
  (`struct mfs_dirent`), so `ls` needs no second lookup.
* The two numbers cross the VFS as well: `struct vfs_stat` carries the mode
  and the owner, `vfs_chmod()` and `vfs_chown()` are operations a filesystem
  may have or not, and a filesystem that has none answers "not permitted"
  instead of storing a number that nothing would read. A physical filesystem
  always has them; `procfs` and `devfs` never will.

The volume of 0.3.x is not rewritten: see "The mistakes that cost time".

### The mode of the object

The check happens at the edge, with the credential of the caller, and the
kernel has one user (uid 0, `KSYS_UID`):

* the bits that decide for the caller are the triad of the owner when the
  object belongs to uid 0 and the triad of the others otherwise - there is no
  group and no third party;
* there is no bypass for the owner of the system: the exemption of root is
  the one part of the POSIX rule that is left out, so a file of mode `0200`
  cannot be read by anybody, the only user included (ADR 0005);
* `open` compares the wish in its flags with the bits of the object
  (`open_path_mode_check()` describes the object with `vfs_stat()` and
  `open_mode_check()` compares the access), and a file that is not there is
  not checked: with `O_CREAT` it is about to be made with the mode of the
  design, and without `O_CREAT` the open itself answers `ENOENT`;
* `execve` reads the same triad and needs the execute bit; a directory, a
  device and a file without `x` are refused before the image is read;
* a zeroed mode means "this filesystem keeps no modes", not "nobody may touch
  it": the virtual filesystems answer through the same door and keep the
  access they always had;
* `chmod` and `chown` are numbers 23 and 24, checked against `KSYS_MODE_MASK`
  and `KSYS_UID_MAX` at the door and stored by the record. A zero mode passes
  the door and is refused by the filesystem, because zero already means "the
  record predates the modes".

The mode of a file is a number of the record and not of the mount: two
objects of the same directory may answer differently to the same caller, and
a `chmod` costs one record and a sync.

`stat` reports the two numbers and checks nothing: it is the call a program
uses to learn what it may do, and POSIX makes it readable to anybody. The
check happens where the access is asked for, in `open` and in `execve`.

### The syscall table is the only door

With programs the kernel did not compile, every argument is checked where it
enters - in `components/kernel/src/ksys.c`, in one function per kind of
argument:

* **A path** (`path_check`): not NULL, and it begins at the root, because every
  filesystem of the tree hangs below `/`. The search for the terminator stops
  at `KSYS_PATH_MAX` (`VFS_PATH_MAX - 1`, so 127 characters); a path that does
  not end inside that window is refused before any filesystem sees it.
* **A buffer** (`buffer_check`): NULL with a length is not a buffer, a zero
  length with NULL is not an error, and no call touches more than
  `KSYS_MAX_IO` (64 KB) in one exchange - far above any buffer of the system,
  and a number that cannot be a real one on a machine with ~100 KB of RAM.
* **The flags of an `open`** (`open_flags_check`): one of the three access
  modes - the pair of the bits is not a mode - and no bit outside
  `KSYS_O_ALLOWED`. The flags are checked before the file is looked for, so
  the answer is about the argument and not about the object.
* **The `whence` of `lseek`** and **the pid of `waitpid`** are compared before
  the cast to `int` can cut a wide register down to a valid value: `2^32 + 1`
  is not `SEEK_CUR`, and `2^32 + 5` is not the child 5. Only 0 and -1 of
  `waitpid` mean "any child", and its flags are one bit.
* **`readdir`** takes whole entries: a count that is not a multiple of
  `struct vfs_dirent` would let the last entry land past the memory of the
  caller.
* **`stat` and `getcwd`** need the place for the answer; NULL there is a
  mistake of the caller, not "the answer is not wanted".

The same functions decide the access of an object (see "The mode of the
object") and load a program (see "The loader"). The table of the calls is the
only door, so everything a program may not do is refused here and not three
layers below, in a filesystem that happens to have the number.

## The tests are about the properties

* **The parser is tested with images made byte by byte in the test**
  (`tests/cases/test_exec.c`), so a change of the layout is visible there and
  not in a file that happened to be built with the same mistake. Every
  refusal has its own case: a wrong magic, a wrong version, an unknown flag, a
  reserved word, a size that does not fit, a size that is not a multiple of
  four, no text, an entry in the header, an entry in the data, a bss above the
  ceiling, a sum that would wrap - and the boundary that must still work (a
  bss of exactly `K_EXEC_BSS_MAX`, an entry one byte inside the text).
* **A machine that cannot run a program says so**, and saying so does not
  touch the image: the host returns `-2` and the bytes and the parsed
  structure are unchanged. A call with no image at all is `-1` (a mistake of
  the caller) and not `ENOSYS` (a property of the machine).
* **`chmod` and `chown` change the nine and the fifteen bits and nothing
  else** (`tests/cases/test_ksys.c`, `tests/cases/test_mfs.c`): the type stays
  where it was, `chmod` is not a way to make a file out of a directory, the
  mode of a directory is its own, and a number that does not fit is refused
  with the record keeping what it had (`KSYS_MODE_MASK + 1`, `MFS_MODE_MASK +
  1`, `01000`, `0xFFFFFFFF`, `KSYS_UID_MAX + 1`, `MFS_OWNER_MASK + 1`).
* **The mode decides what an `open` may ask for**, at the edge and before a
  descriptor exists: a file of `0400` cannot be opened for writing and a file
  of `0200` cannot be opened for reading, the triad of another owner is the
  triad of the others, and a file that is about to be created with
  `O_CREAT` is not checked.
* **`execve` answers for each reason of its own**
  (`tests/cases/test_ksys_process.c`): `ENOENT` for a path that is not there,
  `EISDIR` for a directory, `EPERM` for a real image of mode `0644`, `EINVAL`
  for a file that is not an image and for one too small to hold the header,
  and `ENOSYS` for a real image on the simulator. The raw call through
  `ksys_call()` answers the same, and a failed exec leaves the table of
  descriptors as it was.
* **The library answers the same way in the codes of POSIX**
  (`tests/cases/test_libc.c`): the wrappers change the mode and the owner that
  `stat` reports, clear `errno` on success, report `EPERM` for the forbidden
  `open`, `EINVAL` for a number wider than the record and `ENOENT` for an
  object that is not there; `execve` reports `ENOENT`, `EISDIR`, `EINVAL` and
  the `EINVAL` of a NULL path.
* **The commands of the shell speak to the ABI and not to the filesystem**
  (`tests/cases/test_kshell.c`): `chmod 0711 /bin` and `chown 7 /bin` are the
  numbers the next `vfs_stat()` reports, `0999` and a uid above the fifteen
  bits are refused before a call is made, `chmod 000` prints the reason the
  filesystem gives, and `exec` prints the reason of the ABI for a path that is
  not there.
* **A volume written before the modes still mounts**: a record with the type
  bits only is read as `0755` for a directory and `0644` for a file, by the
  reader and not by a rewrite, a listing reads the same fallback, a mode that
  is on the volume wins over it, and the owner can be filled in on such a
  record.
* **The mode and the owner survive a remount**: they are part of the record,
  so they are on the volume before the restart, like the data
  (`tests/cases/test_mfs_persistence.c`).
* **The mode and the owner travel through the VFS** (`tests/cases/test_vfs.c`):
  `vfs_stat()` reports the two numbers a filesystem keeps, a `chmod` does not
  move the type and a `chown` does not move the mode, and a number wider than
  the record is refused where the two contracts meet.
* **The negative cases of the edge**: NULL and empty paths, a relative path, an
  unterminated buffer of `a`, one byte more than the window, and the bound
  itself (127 characters) which is still a path and reaches the filesystem;
  NULL with a length on a file and on the console behind fd 1; `KSYS_MAX_IO +
  1`; the `whence` of `-1` and `SEEK_END + 1`; the wide numbers through the
  raw `ksys_call`; the flags that no bit owns and both access bits at once,
  refused before a descriptor is spent and before a filesystem is asked; a
  `readdir` count that is not a multiple of an entry; `waitpid(-2)`,
  `waitpid(0, 0x0100)` and a pid that does not survive the round trip through
  `int`.
* **The contract is closed**: the three calls of `exec` are in the table of
  `tests/cases/test_port.c` with the rest of the contract, and every port
  implements them.

What the host tests cannot prove is a program that really runs: the simulator
cannot enter one, so the branch where a program is read from a file, writes and
comes back is proved by `make x86-smoke` on the port, at the boot of that
machine and not through the call. The x bit of the directories of a path is
not tested because it is not checked at all (see "What is still missing").

## The mistakes that cost time

* **The magic was written as a number and then read the other way.** The four
  bytes of the file are `M`, `O`, `X`, `1`; as a little-endian word they are
  `0x31584F4D`. A design note that keeps one form and a parser that keeps the
  other disagree in silence, and the symptom is a refusal of every image with
  no hint of which field was wrong. The format now names both, and the tests
  build the bytes, not the number.
* **The sum of the sizes almost wrapped.** `header + text + data` in 32 bits
  passes for an image that claims `0xFFFFFFFC` of code and would make the
  parser read the memory behind the file. The sum is 64 bits wide, and the
  test has the wrapping image.
* **A cast to `int` made a wrong number valid.** The arguments of a system
  call arrive as raw registers of the target's width: on the 64-bit host
  `0x100000000 + SEEK_END` survives the cast as `SEEK_END`, and
  `0x100000005` as the child 5. Both are compared before the cast now, with a
  test that calls `ksys_call` with exactly those numbers.
* **A length is not a memory range.** Without an MMU the kernel cannot know
  which memory a pointer of the caller names, so it cannot check the range the
  way Linux does. What it can do is bound what one call may touch
  (`KSYS_MAX_IO`) and refuse NULL with a length; the test of a pointer into
  the memory of the kernel stays impossible until a port has an MMU, and the
  document says so instead of pretending.
* **The zero of the mode was already taken.** A record of a volume of 0.3.x
  has nothing in the bits of the mode, so zero has to mean "the record
  predates the modes" and the reader falls back to `0644`/`0755`. The price is
  that `chmod 000` cannot be expressed: it is refused (`MFS_EINVAL`) instead
  of being stored and read back as `0644`.
* **The padding was written once, at the end of the file.** The format pads
  every segment to four bytes, and a program is linked for the address where
  the padded text ends; `mkprog.sh` padded the whole file after the data, so
  the data sat one to three bytes before the address the program read it from.
  The image was a valid program, it ran, and it printed the wrong bytes - the
  kind of failure that looks like a bug in the program. The script now pads
  each segment before the next one starts.
* **The directory entry has one byte for the type.** It keeps the type bits of
  the record and not a copy of the mode and the owner: those are read from the
  record when the listing is made, so a `chmod` cannot leave a stale number in
  a directory.
* **A library of a machine without a thread pointer.** `errno` is a
  `_Thread_local`, and a freestanding port has to put a block under `%fs`
  before the library writes it: the x86-64 port set no `IA32_FS_BASE`, so the
  first `chmod`, `chown` or `exec` of the shell wrote to address zero and
  faulted. The kernel does not use the library, which is why nothing before
  the shell had noticed.

## What is worth remembering

* The format belongs to the tree that ships the loader. A wrapper of `objcopy`
  and 32 bytes of header cost a page of documentation; the loader of ELF costs
  a chapter, and the smallest machine of the project has 100 KB of SRAM for
  everything, drivers included.
* A machine that cannot do something must say so early and in one place
  (`k_port_exec_supported()`), so that the answer is `ENOSYS` at the edge
  instead of a crash far from the cause. The board is not a lesser machine for
  it: it is the machine where the permissions are real.
* The parser is tested where it cannot run what it parses. That is why the
  host build covers the format while the jump of the x86-64 port is a separate
  milestone with its own program.
* Validation at the edge is one small function per kind of argument, so a new
  call answers like the old ones and the negative cases can be written once
  for the whole table.
* A format that fits into the spare bits of an existing record is a format
  nobody has to migrate. The volume of 0.3.x mounts, keeps its data, and gains
  modes by being read.

## What is still missing

* **A program calls the kernel, but from ring 0.** The x86-64 port puts
  `int $0x80` on vector 128, passes six arguments in registers and returns a
  status; the ABI is in [ADR 0006](../adr/0006-program-entry.md) and
  `docs/syscall-abi.md`. The program still runs without an address space of
  its own, the handler trusts the pointers it passes, and a program that does
  not return holds the machine: ring 3, a kernel stack per task and an `exit`
  call wait for K5. See [debts.md](../debts.md).
* **`execve` runs a program only on x86-64.** The boot writes `/bin/hello`
  and asks for it with `ksys_execve()`, and the shell does the same with
  `exec`; the check of the mode, the read into `kmm` and the answer to the
  caller are the ones of the call. The host and the board answer `ENOSYS`,
  which the tests cover.
* **The first program lives on one machine and for one boot.** The volume that
  holds `/bin/hello` is an array in the RAM of the x86-64 port, and the port
  formats it and fills it at every boot; the board and the simulator get no
  `/bin`. The smoke test of the port does drive the file through the shell:
  `chmod 0644` makes `exec` refuse with `EPERM`, `chmod 0755` gives it back,
  and `exec` runs the program and reports that it has finished.
* **`PATH` is missing.** `exec` takes the path it is given, resolved against
  the working directory; there is no list of directories to search.
* **`argv` does not reach the port.** `sys_execve()` takes the vector and
  marks it unused, `k_exec_run()` has no place for it, and the port is
  entered with NULL. The ABI of the entry - the registers, the stack, its
  alignment, where the strings live - is not written down. The kernel does not
  walk the array of the caller without an MMU, and that is another reason the
  vector is the port's business.
* **The image is read whole into the kernel.** A program is at most one
  `KSYS_MAX_IO` (64 KB) because the loader reads it in a single block of
  `kmm` before the port copies it to the window - two copies in RAM at once,
  and the size of the image is checked against the transfer limit of the edge
  and not against the memory of the machine.
* **`stat` checks nothing.** It reports the mode and the owner to anybody who
  asks, as in Linux, and the check happens where the access is asked for, in
  `open` and in `execve`.
* **The mode of a directory does not decide the search.** The check happens
  for the object a call names, not for every directory of its path: there is
  no `x` bit walked on the way, so a directory of mode `0600` still lets its
  files be opened by their full path.
* **`fork()` with an address space of its own is not here.**
  `k_port_fork_copy()` exists only in the ADR; the board keeps the placeholder
  of phase 1 (`KERNEL_NOMMU`), and the page tables per task with copy on write
  are the milestone of the paging of the x86-64 port.
* **Two error codes are missing or wrong.** A path over the window answers
  `KERR_TOOBIG` (`E2BIG`) because the contract has no `ENAMETOOLONG`, and a
  `waitpid` for a child that never comes answers `KERR_AGAIN` after five
  seconds where POSIX says `ECHILD` or `ESRCH`. `ESRCH` also needs a decision
  about its number: `-3` is taken in `kernel/errno.h` by `KERR_EXIST`, which is
  not the POSIX value of `EEXIST` either.
* **A directory cannot be opened.** `mfs_open` answers `MFS_EISDIR`, so
  `readdir` - which takes the descriptor of an open file - has no path from a
  program; the shell lists a directory through `vfs_list()` itself.
* **`mkprog.sh` does not check the address a program is linked for.** It reads
  the VMA of the entry symbol and of `.text` to compute the offset, and the
  port ships a linker script (`programs/program.ld`) that puts `.text` at the
  base of the window plus the header, but the script does not compare the two:
  an image linked for another address is refused at the run, or worse. A check
  of the first address of `.text` against `k_port_exec_base()` is missing.
* **The padding of the segments has no test.** The script pads every segment
  before the next one and the smoke test of the port notices a wrong byte, but
  the output of `mkprog.sh` - the bytes a program reads its data from - is not
  checked by a test of the host. The bug that cost a wrong byte is fixed and
  not guarded.
* **The board will never run a program**, and that is a decision of ADR 0005
  and not a debt: its flash is not executable for a file, and a copy in
  internal SRAM would take the memory of the kernel and the drivers.
