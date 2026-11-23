# Changelog

The changes of Micro OS, one section per release, newest first. The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) under the rule of
[ADR 0011](docs/adr/0011-versioning-and-1.0.md): before 1.0 the minor number
was a phase and could break a format; from 1.0 the syscall numbers, MOX1 and
the MicroFS volume change by adding only.

A release is the commit a tag names (`vX.Y.Z`); a tag with a dash
(`vX.Y.Z-pre`) is a pre-release and must match `kernel_version()`.

## [Unreleased]

### Added

- A volume of 5 GiB is an ordinary volume: `make x86-volume-smoke` writes the
  images of ext2 and of MicroFS of the plan, boots each as the root of the
  machine, writes a file of 128 MiB (16 MiB of MicroFS) and reads it back, and
  then reads the image of the session with the tools of the world - `e2fsck
  -fn` finds nothing to repair and `debugfs` sees the file, while the `fsck`
  of the machine says the MicroFS volume is consistent. The image of ext2 is
  sparse on the host (6 MB of a disk of 5 GiB), the file goes in at about
  1.2 MiB/s and comes back at 4 MiB/s, and the map of the free blocks of the
  MicroFS volume is 615 KiB of the heap. The target is not a part of `make ci`
  (about four minutes) and `docs/wiki/files.md` has the numbers.
- ext2 writes the double and the triple table of pointers: a file grows as far
  as the format allows it (2 GiB of blocks of 1 KiB), so a file of 128 MiB of
  a 5 GiB volume is one `write` after another and not the answer of a full
  volume. [[`components/fs_ext2/src/ext2_write.c`]]
- The superblock of an ext2 volume settles its counters of the free blocks and
  the free inodes when an operation that allocates or frees is done (a close,
  a create, a `mkdir`, an `unlink`, a cut): the root of the machine is never
  unmounted, and `e2fsck -fn` of the host now finds a sound image after a
  session instead of a superblock that lies about the volume.
- `make sdk` writes the SDK of a program into `build-sdk/microos-sdk/<version>/`
  (or `DESTDIR`): the headers of the library and of the ABI, the archive
  `lib/libmicroos_user.a` built from those sources (35 objects, arena of
  256 KiB, `-Os -ffreestanding -mno-red-zone -march=x86-64`), `crt0.o`,
  `program.ld`, `mkprog.sh`, `link_program.sh`, `microos_sign`,
  `share/microos-toolchain.cmake`, `share/build-libc.sh` with the sources,
  and `share/examples/hello`. A program is built outside this tree with two
  commands - `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/...` and
  `cmake --build build` - and no include path or library of its own
  (`docs/porting-programs.md` §6, ADR 0025 §6). The example is 10 688 bytes
  and runs from `/bin` on the machine of QEMU.

### Fixed

- MicroFS wrote a file of 16 MiB of a 5 GiB volume at 0.11 MiB/s: the growth
  of the chain moved the cursor of the position of the descriptor, so a write
  that grew the chain to the end of the call then walked it again from the
  head for the block it was about to write - the square of the size of the
  file. The append has a cursor of its own and the same file goes in at
  1.9 MiB/s.
- The walk of the table of the records of MicroFS read the block of the table
  once per record (32 times the bytes of the table): `df`, `fsck` and the
  line of the mount walked the 214 MB of the table of a 5 GiB volume 32 times.
  One block of a cache in the filesystem makes the walk cost the table.
- The wait of `execve` for the program it started was ten seconds, which a
  file of 128 MiB of a large volume outran: the caller returned to the shell
  while the program still wrote, and the next line of the shell was a second
  writer of the same file - a volume that needed `e2fsck`. The wait is ten
  minutes, and a program that outlasts it is told to stop with SIGINT and
  given a second to answer before the caller gives up.

## [1.3.0] - 2026-11-23

The release of the many cores. `KERNEL_SMP` stays off by default: with it off
the kernel is the kernel of `1.2.0` in behaviour and in its tests, and with it
on the machine of QEMU starts both of the processors its firmware describes
and runs tasks on each of them. The board is not part of this release (ADR
0018); the many cores are x86-64 and the host substrate.

### Added

- The x86-64 port starts the application processors (ADR 0012). The port
  keeps the MADT list with the hardware identifier of every processor, sends
  INIT and the start-up IPI through the local APIC to the trampoline the
  loader carries at `0x7000`, and gives every core a 64 KiB stack, a GDT, a
  TSS and an IST of its own. A core reports itself from C before the
  scheduler exists, waits there, and `kmain()` releases the cores once
  `kernel_boot()` has made the array of the kernel; a core that never
  answers is not counted. The boot line is
  `x86-64: 2 cpu(s) reported by the firmware, 2 in use (apic 0xfee00000)`.
- The local APIC of every core (src/lapic.c, src/ipi.c): the page mapped in
  the top 2 GiB, the spurious vector, the timer calibrated against the TSC
  (QEMU measures about 62.6 MHz at divider 16, i.e. a millisecond count of
  62 600), the EOI, the command register of INIT and the start-up IPI, and
  the vector of an inter-processor interrupt. The I/O APIC is still a stub
  (see `docs/debts.md`, K8).
- The identity of a core is one load: IA32_GS_BASE points at a record whose
  first word is the kernel number, and `k_port_cpu_id()` is `movl %gs:0`.
  The boot is found by CPUID, the others follow the MADT; the boot has the
  number 0.
- `smp`, `smp workers <n>` and `smp ping` in the shell of the port, only in a
  `KERNEL_SMP` build: the cores in use, the ticks of every core, the task of
  every core, the queues checked, tasks placed on both cores, and an IPI that
  arrives on the other core and is counted there.
- The task substrate of the port is per core (`struct port_cpu`): the current
  context, the context of the core before its first task, the pending stack
  and address space of a dead task, and the thread pointer. An exit with
  nothing else to run gives the core back to its boot or idle context, which
  runs `schedule()` in a loop.
- The host gate of the many cores: `make test-smp` (the whole suite on two
  cores), `make test-tsan`, `make soak-host`; `tests/cases/test_smp.c` gains
  the tests of the claim and of the state that one task on two cores is.
- `make x86-smp-build`, `make x86-smp-run`, `make x86-smp-smoke` (34 checks:
  both cores in use, a worker on each, the AP clock moving, the IPI, the
  queues before and after the load, and no exception) and `make soak-smp`
  (the 600-second session on the two-core image, with the ticks of both
  cores sampled every cycle).
- `pkg` knows the license of a package (ADR 0025). An entry of the index
  carries `license` (the SPDX string), `source` and `source_sha256`; a
  copyleft package (`GPL-`, `LGPL-`, `AGPL-`, `CDDL-`, in either case)
  without a source and the digest of its archive is refused with the new
  answer `PKG_ELICENSE` (-13), and the message names the license and the
  package. `pkg info` prints `License` and `Source` for a repository package
  (`-Si`) and for an installed one (`-Qi`), the record keeping both fields;
  `-U` takes a local file and is not asked, because it has no index behind
  it.
- The ports of the third and the fourth library of somebody else: zstd 1.5.7
  (BSD-3-Clause) as the archive of the frame format and `/bin/zstd` of its own
  command, and file 5.45 with libmagic (BSD-2-Clause) as `/bin/file` with the
  compiled subset of the magic database in `/etc/magic.mgc`. The library of a
  program learned the two headers of the fixed-width numbers (`stdint.h`,
  `inttypes.h`), `asprintf`/`vasprintf`, the precision of a string in the
  formatter, `pread`, `asctime`/`ctime`, and one more name of a compressed
  stream: `/bin/file -z` looks inside a zstd frame as well as a gzip. The
  library is one implementation per name for every port: the POSIX families
  of sbase and the formatter with a point of Lua.
- SQLite 3.54.1 (the public domain) as `/bin/sqlite3` (846,080 bytes, the
  largest image of the machine) over a VFS of the port, with the scripts of
  `/etc` as its fixtures. The library of a program took `sscanf`, `gettimeofday`,
  `getrusage`, `usleep` and the refusals of `realpath`/`popen`/`setenv`, and
  one implementation per name stayed the rule: a call of the tree is not
  redefined by a port.
- The eighth port, and the first interpreter with a grammar of its own: the
  One True Awk of 2024-07-28 (the Lucent permissive license) as `/bin/awk`
  (177,056 bytes), with the data `/etc/columns`, the first script of the
  machine `/etc/hello.awk` and the license at `/etc/licenses/awk`. The
  release is vendored unchanged in `third_party/awk` (the parser is generated
  once by byacc and committed with its header and `proctab.c`), and the port
  layer is `port/awk_port.h` and `port/awk_os.c`: the C locale as bytes, and
  the calls with no other home in the tree (`system`, `random`/`srandom`, the
  flag of `fcntl`). A command of a pipe is the `popen` of the library of a
  program, which answers ENOSYS; `system` says on the error descriptor that
  this machine has no shell. `make x86-smoke` runs the fields,
  the formatter, `-v`, `-f`, `getline` and the refusals; `make
  x86-disk-smoke` runs the script of `/etc` again after the reset; the host
  test `tests/cases/test_awk.c` runs the `main` of the release through
  thirty-two cases, including the comparison of a numeric string and the
  syntax error of a program.

### Changed

- The scheduler claims the next task under its lock: `k_sched_claim_next()`
  marks the task RUNNING on the calling core in the same critical section
  that chose it, so a second core cannot claim it between the choice and the
  switch.
- A core picks only from its own run queue. This kernel keeps the task that
  runs in the queue of its core, and a task whose quantum has just expired is
  READY for a few instructions before the switch; a second core that took it
  there would resume a stack that is still in use. A task changes core at
  creation, on wake after a migration, and through `k_sched_migrate()`.
- `k_port_write()` holds the big kernel lock for the whole call: the console
  is one resource of the machine, and the other core could write between two
  bytes of a line.
- The line of a task's end is written before the state that lets its parent
  continue, so a two-core transcript shows a command finishing before the
  next prompt.
- Every core sets `CR0.WP` for itself: without it the kernel's copy into a
  read-only page of a program did not fault on an application processor, the
  copy-on-write never ran, and `/bin/nx` answered differently by core.
- The kernel version is `1.3.0`.
- A read of a large file of MicroFS follows the chain of blocks forward: the
  descriptor asked `chain_block_at` for the block of every chunk, so a read of
  n blocks cost n walks from the head of the chain, and an `exec` of an image
  of 440 KiB spent about six seconds of the emulated machine on it. The block
  that was just read names the next one, and a call walks the chain once for
  its first chunk and then follows it.

### Fixed

- The bss of a program starts at the address the program was linked for:
  `tools/mkprog.sh` counts the gap the linker may leave between the data and
  a bss aligned to 32 bytes as padding of the data segment, and the kernel
  clears the bss where the linker put it. The arguments of a program are
  written after it, so a gap the header did not carry made the first boot of
  awk read its own `argv` out of the state of the random generator. The case
  is in `tools/test_mkprog.py`.

- The 8259 was opened to the external mode on every core, and QEMU delivered
  the timer to either of them (1889 ticks on the core 0 and 4933 on the core
  1 of 6822): the clock of the kernel, advanced only by the core 0, ran at a
  third of real time. LINT0 is ExtINT on the core 0 alone; the APs keep it
  masked and take their ticks from their own APIC timer.
- A core with nothing to run waited in the context of a dead task whose slot
  its parent might reap; the tick then could not find the stack it was on,
  and a preempted task could be left RUNNING while the core idled in the
  context of a stranger. The core returns to its boot context instead.
- The children of a task that ends are reparented to init (pid 1), and the
  init of the x86-64 port reaps them: a program killed by Ctrl+C while it had
  a child left a zombie whose parent no longer existed, and its slot of the
  table of 32 stayed taken for the life of the machine (the two-core soak
  found one 3000 pids old).
- The heap of the port and the pool of the page tables took only the
  interrupts of the core. A tick could preempt a task that held the heap and
  switch to a task that allocated, and the new task spun for ever on the lock
  of the context it interrupted; two cores could take the same page of the
  table pool. Both are now sections of the machine, the heap with the
  interrupts of the core saved around it.

## [1.2.1] - 2026-11-23

A patch of `1.2.0` for the streams of the library, the words of the shell and
the build of the two machines. The volume, the syscall ABI and `MOX1` do not
change.

### Fixed

- `fread` after `ungetc` advanced the pointer it was given and then wrote the
  rest of the block past it: a byte of the caller's buffer was overwritten and
  the data shifted by one. Two programs of `/bin/streams` - the new test -
  prove it on the machine, because a stream needs a kernel under it.
- `fseek(SEEK_CUR)` counted from the descriptor and the block of read-ahead of
  `1.2.0` puts the descriptor ahead of the stream: a relative seek after a
  short read moved by a whole block. `ftell` now counts the byte of `ungetc`
  as well.
- `make x86-user` built the ESP-IDF firmware before it packed the tar, so a
  machine without ESP-IDF could not produce the artifact a person downloads.
  The packing is split in two halves now: `tools/bundle.sh --x86-only` packs
  the x86-64 machine without the firmware build, `esptool` or the partition
  table, and `--board-only` packs the firmware alone. `make bundle-x86` is the
  first half, `make bundle-board` the second, `make bundle` composes them and
  a release workflow keeps one command.
- The `CHANGELOG` of `1.2.0` shipped with the markers of the merge that wrote
  it, and `integrity` had no rule that would notice. It has one now: a line
  that starts with the marker of an unfinished merge fails the check.

## [1.2.0] - 2026-11-23

The release of the machine a person can see and feed: x86-64 gets a real
screen, a stream and an installer of programs, a bootable ISO and a tar that
is the whole machine; the commands of other people arrive as the canonical
`/bin` (sbase), with zlib, PCRE2, Lua and the calculators bc/dc, and the
repository `microos-progs` publishes them with a signed index. The shell
fixes of `1.1.2` are in this release, and the board is the compatibility
port of ADR 0018: it builds, passes its device test and receives no TLS and
no second core. Many cores and the ports that need more than the installer
can carry - SQLite, zstd, `file`/libmagic and awk - are `1.3.0`, not this
release. The notes are in [`docs/releases/1.2.md`](docs/releases/1.2.md).

### Added

- The screen of x86-64 (ADR 0023): the Bochs VBE interface of the VGA of
  QEMU (`1234:1111`, the ports `0x1CE`/`0x1CF`) is programmed to 1280x800,
  16 bits per pixel in RGB565, and the window is mapped above the identity
  map so every address space draws on the same picture. The console is a grid
  of 8x16 cells of Spleen 8x16 2.3.0 (BSD-2-Clause, `third_party/spleen`):
  160 columns by 50 rows, and `font.line_gap` of `/etc/font` makes the line
  18 or 20 pixels (44 or 40 rows). A machine without the interface keeps the
  text mode of 80x25, and `make x86-vga-smoke` reads both screens with
  `screendump` and `pmemsave`.
- A colour per cell (K11c): the sixteen colours of the CGA in RGB565, the
  byte `(paper << 4) | ink` on every cell of the grid and in the attribute of
  the text mode, the SGR sequences of a terminal (`0`, `1`, `22`, `7`, `27`,
  `30`-`37`, `39`, `40`-`47`, `49`, `90`-`97`, `100`-`107`) in the portable
  `kernel/ansi.c`, the levels of `klog` (warn amber, error red, success
  green), `console.ink`/`console.paper` of `/etc/console` and `ls --color`.
  The serial copy stays a plain transcript.
- `make x86-iso` (ADR 0026): an El Torito ISO (BIOS) with the loader, the
  signed kernel and the root image, written with GRUB when `grub-mkrescue`
  is installed and with limine otherwise. The root image is the second module
  of the boot, the port registers it as the read-only device `boot0`, and
  `make x86-iso-smoke` boots the ISO with `-cdrom` and nothing else, mounts
  the volume and runs `/bin/hello`.
- `make x86-user`: the complete machine and the x86-64 tar of the release,
  unpacked and started from the tar itself, so the artifact a person
  downloads is the one that runs. `make x86-user-serial` is its console.
  The x86-64 tar now carries `x86-all.img`, the disk of `/bin` and `/etc`,
  and a `run.sh` with the disk and the network; the release workflow builds
  the complete machine before the bundle.
- TLS on x86-64 (ADR 0024 §3): Mbed TLS 3.6.2 vendored in
  `third_party/mbedtls` with a client-only configuration, and a freestanding
  platform layer - `kmm` for the allocator, the formatter of the kernel,
  `RDRAND` with the timer as the fallback for entropy, the clock of NTP for
  the dates, and the roots of `/etc/ssl/certs.pem` read through the VFS.
  `http_parse_url()` takes `https`, the stream wraps the plain transport, and
  `wget`, `http` and `pkg` speak HTTPS with the chain and the name checked; a
  clock that was never set is a refusal with a sentence, never a silent
  success.
- `ntp <server>[:port]` takes a port other than 123, for a home server and
  for the clock of a test.
- The x86-64 port registers a software clock, so `ntp` has a clock to set on
  a machine without a driver of the CMOS chip yet.
- `tests/cases/test_tls.c`: the policy of the clock, a store of roots of one
  and of two certificates, a broken store, and the layer that has no
  transport under it; `make x86-net-smoke` generates a test authority and
  three certificates with `openssl` and checks TLS 1.3 and 1.2, the refusal
  of a wrong name and of a wrong authority, and `pkg` over HTTPS.
- `pcre2grep`, a program of x86-64 that matches the lines of a file against a
  regular expression: `-i`, `-v`, `-n`, `-o` and `-g N`, with `^`/`$` as the
  ends of the line and numbered groups. It links PCRE2 10.42, vendored in
  `third_party/pcre2` (BSD-3-Clause WITH PCRE2-exception), the 8-bit
  interpreter without JIT and Unicode.
- The library of a program gained the ASCII character classes of the C
  locale (`<ctype.h>`), which the engine of a regular expression asks for.
- `tests/cases/test_pcre2.c`: the matches, the groups, the anchors, the
  UTF-8 bytes of a build without Unicode, the compiler errors and the values
  of the vendored `config.h`.
- `docs/licensing.md` and the license of PCRE2 in the bundle and on the disk
  (`/etc/licenses/pcre2`).
- The second port of a program of somebody else: Lua 5.4.7 (MIT) as a static
  archive of the guest and `/bin/lua`, with the first script of the machine
  `/etc/hello.lua` and the license at `/etc/licenses/lua`. The release is
  vendored unchanged in `third_party/lua`; the settings of the system are one
  force-included header (`port/luaconf_port.h`: no shell, no `popen`, no
  `tmpnam`, no dynamic linker, the C radix) and its `port/lua_os.c`. The
  recipe is a new section of `docs/porting-programs.md`.
- The library of a program learned the last pieces a language asks for: the
  conversions `%f`/`%F`/`%e`/`%E`/`%g`/`%G` and a precision, `fprintf`/
  `vfprintf`/`sprintf`, `strtod`/`strtoll`/`strtoull`/`qsort`/`getenv`,
  `setjmp`/`longjmp` in the registers of x86-64, `math.h` on the x87 unit,
  `time`/`localtime`/`mktime`/`strftime`, `locale`, `ctype`, `assert`,
  `signal`/`raise`, and `remove`/`rename`/`fileno`/`ungetc`/`freopen`. A
  program that computes in `double` is built without `-mgeneral-regs-only`
  (the `FLOAT` option of `cmake/userland.cmake`), and the loader sets
  `CR4.OSFXSR`, `CR4.OSXMMEXCPT` and `CR0.NE` for it. The host test
  `tests/cases/test_lua.c` runs the language, its formatter, its tables, an
  error caught by `pcall` and a script file.
- The image of `execve` is padded to sixteen bytes now: a constant of a
  program with floating point is a sixteen-byte object and `movaps`/`andps`
  fault when it is not aligned. `tools/mkprog.sh` pads both segments to
  sixteen for the contiguous window of x86-64 and keeps the four bytes of the
  split windows of the board.
- `execve` reads an image of up to 8 MiB (`K_EXEC_MAX_IMAGE`), in pieces of
  `KSYS_MAX_IO` (64 KiB) into one block of the allocator, and a larger file
  is `E2BIG` before a byte of it is read. The transfer edge of one call was
  the ceiling of a program; `/bin/lua` (164 KiB) is the first one past it,
  and `zcat` (58 KiB) is unchanged. The syscall ABI, the numbers and MOX1 do
  not change; `make x86-smoke` runs the interpreter and boots a disk with a
  file of 8 MiB + 1 to check the refusal, and the host test
  `tests/cases/test_exec.c` checks the boundary of the constant.
- The fourth port of somebody else's code: bc 7.2.0 (BSD-2-Clause) as a
  static archive and the two names `/bin/bc` and `/bin/dc` of the same
  engine, with the scripts `/etc/bc.bc` and `/etc/dc.dc` and
  `/etc/licenses/bc` in the image. The release is configured by the
  force-included `port/bc_port.h` (no history, no NLS, 64-bit limbs) and its
  two POSIX calls are answered by the port layer (`fstat` of a descriptor and
  `sigaction`). The library of a program learned `strdup`, `srand` and
  `rand`; the host test is `tests/cases/test_bc.c`, and `make x86-smoke` and
  `make x86-disk-smoke` run the calculator and the RPN one before and after
  the reset. `bc -l` and `4*a(1)` answer `3.1415926532` at a scale of ten.
- `make x86-smoke` runs `/bin/lua`: a power of a double (`1024.0`), `math.pi`,
  `string.format("%.3f", 1/3)`, the two values that are not numbers, the
  clock, and the script `/etc/hello.lua` with a table, a file of the VFS and
  the strings. `make x86-disk-smoke` runs the same interpreter out of the
  volume in the first boot and after the reset.
- `microos-progs`, the public repository of the programs, with a signed
  index at <https://turbososiska304.github.io/microos-progs/>: the index and
  every `.mox` carry the Ed25519 signature of the project, the key is
  `microos-progs.pub` (and `/etc/pkg/keys/microos-progs.pub`), and a machine
  adds it with `pkg repo add` and installs from it with `pkg -S`.
- `make x86-disk-smoke` grew to the programs of the volume: the interpreter
  of Lua and its script, the calculator and the commands of sbase run in the
  first boot and after the reset.

### Changed

- `pkg` takes a package of up to 8 MiB: `PKG_PACKAGE_MAX` is the ceiling of
  an image of a program (`K_EXEC_MAX_IMAGE`) and not a number of its own -
  where `kernel/exec.h` names the ceiling, the installer uses the name - and
  the install, the parse of the index and a local file ask one
  `pkg_size_check()`. A package between one and eight megabytes is no longer
  refused by the installer itself, and a file one byte past the ceiling is
  refused with the size in the sentence.
  `tests/cases/test_pkg.c` checks both sides of the boundary.
- `struct http_transport::open` receives the transport itself, so a layer can
  wrap the transport below it; a plain transport ignores the argument.
- `struct http_url` names the scheme: `tls` says the URL is https, and a
  redirect keeps the scheme of the hop.
- The first port of a program of somebody else: zlib 1.3.1 as a static
  archive built by the compiler and the flags of the guest, and `/bin/zcat`
  on a `.gz` of MicroFS. The recipe is `docs/porting-programs.md`; the
  library is in `third_party/zlib` with its port layer and license, the
  image carries `/etc/greeting.gz` and `/etc/licenses/zlib`, and
  `make x86-smoke` and `make x86-disk-smoke` check every inflated byte.
- The library of a program learned the three families a portable source
  expects: an arena of its own in the bss (`malloc`, `calloc`, `realloc`,
  `free`), `errno`/`strerror`, and the streams of `fopen`; the POSIX names of
  the headers are thin shims over `ulibc.h`, and `snprintf`/`vsnprintf` share
  the formatter with `printf`. The host test `tests/cases/test_zlib.c` covers
  the checksums, the three stream wrappers and a `gz` file.
- `make board-soak` and the gate of a compatibility release: the session on
  the board is three minutes (`BOARD_SOAK_MINUTES`), and the long ten-minute
  session belongs to a release that touches the flash or the boot.
- The x86-64 image has twenty-six programs: `pcre2grep`, its engine, `lua`,
  `bc`, `dc` and the ten commands of sbase join the eleven of the tree and
  `zcat`; `make x86-smoke` checks their lines, the five licenses and the
  count.
- The third port of somebody else's code, and the first set of commands:
  sbase (MIT) with `cat`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`,
  `xargs` and `sed` under their canonical names in `/bin` of the x86-64
  image, in place of the small `ls`, `cat`, `grep`, `wc` and `head` of ADR
  0016; `echo`, `mkdir`, `pwd` and `rm` stay. The `readdir` of procfs and
  devfs makes `ls /proc` and `ls /dev` work with the same call as a directory
  of MicroFS, and the image carries the two mount points as empty
  directories. The library of a program grew the families a POSIX command
  expects (`opendir`/`readdir`, `struct stat`, `getpwuid`, `localtime`,
  `fnmatch`, a regular expression engine, `getopt`, `getline`, `fmemopen`,
  `qsort`) and a directory of its own for the paths, because the ABI has no
  `chdir`. The port layer and the hunks of `sort.c`, `xargs.c`, `grep.c` and
  `ls.c` are in `third_party/sbase/README.md`; `xargs` runs its command in
  its own process because `execve` is synchronous, and `-P` has nothing to
  do. The shell looks a bare name of the ten in
  `PATH` before its own table; the simulator keeps the commands of the shell
  and the board falls back. `ls --color` paints a directory blue and an
  executable green. The pure library is tested by
  `tests/cases/test_userland.c`, and the ten tools are in `make x86-smoke`
  and `make x86-disk-smoke`. The MIT notice travels as
  `/etc/licenses/sbase` and `LICENSE.sbase`.
- A program with a point in its numbers is a program of its own: the port's
  loader sets `CR4.OSFXSR`/`CR4.OSXMMEXCPT` and `CR0.NE`, the runtime of the
  image is built without `-mgeneral-regs-only`, and `math.c`, `stdlib.c`,
  `setjmp.c`, `time.c` and `format_float.c` are linked into it. The
  scheduler saves no XMM or x87 state and `execve` waits for the program, so
  one program with a `double` runs at a time.

### Fixed

- `libc`: a stream of a program reads a block of 512 bytes from its descriptor
  instead of one byte at a time, so the line readers of the release stop being
  a system call per byte: `wc -c` of a 168 KB file was 11.6 s and is 0.13 s,
  `grep` over a file of a few hundred kilobytes no longer trips the ten-second
  patience of the shell. The block lives in the `FILE` object (no allocation),
  `fseek` drops it and `ftell` counts what is still in it; the write side is
  unchanged.
- `mfs`: a read of a file follows the chain from the cursor of its descriptor
  (`chain_block`/`chain_index` in `struct mfs_file`) instead of walking from
  the head for every block: a read of 540 KB is 28.6 s before and 0.18 s
  after, `exec /bin/lua` (168,480 bytes) is 0.09 s instead of about a second,
  and the `write` path is the next debt of the same kind (`docs/debts.md`).
  The format on the disk does not change.
- `esp32`: the two programs of the board link the `errno` of the library and
  the compiler's runtime (`-lgcc`). The merge of the zlib userland put an
  `errno` in the wrappers of the calls and the 64-bit division of the
  formatter in `stdio.c`; the board's link had neither, so `make idf-build`
  stopped at `/bin/hello`. The text of a program is 4540 bytes of the 8 KiB
  window.
- The shell fixes of `1.1.2` are in this release: `ls` and `tree` stream a
  directory, `shell_resolve` normalises `.` and `..`, `help <command>` is not
  cut, `path ""` is refused, and the console answers with its own sentences.
  The patch has its own section below and its notes in
  [`docs/releases/1.1.2.md`](docs/releases/1.1.2.md).

## [1.1.2] - 2026-11-23

A patch of `1.1.1` for the shell: the cleanroom of 1.1.1 found that `ls`
stopped at 64 names, `tree` at 16, and that a path with `.` or `..` was not
a path. The volume, the syscall ABI and `MOX1` do not change; the notes are
in [`docs/releases/1.1.2.md`](docs/releases/1.1.2.md).

### Fixed

- `ls` and `tree` read the directory through `readdir` in pieces and print as
  they go; the arrays of 64 and 16 names are gone. `tree -d` counts the files
  it does not draw, and the depth beyond six is counted as `(deeper)`.
- `shell_resolve` normalises `.`, `..`, `//` and a trailing `/`; `cd ..`,
  `cd .`, `ls ..` and `cd /proc/..` work, and `/..` is `/`.
- `help <command>` writes a long detail without the 192-byte line buffer, so
  the sentence is not cut in the middle.
- `path ""` is refused (reset with `path /bin:/usr/bin`); `umount /proc` and
  `/dev` are refused while they are mounted; `df` of a path that is not there
  is `no such file or directory`; `config` on a machine without a store says
  so and `config sync` fails instead of a false `0 saved`.
- `memtest` repeats an operation until the interval is measurable; a block
  too fast for the timer is `too fast to measure`, not a hundred times the
  speed of the memory.

### Changed

- The order of `ls` is the order of the filesystem: a sorted listing needs
  the whole directory in memory, which is the limit this patch removes. The
  note in [`docs/debts.md`](docs/debts.md) records the trade.

## [1.1.1] - 2026-11-22

A patch of `1.1.0` for the data loss that the cleanroom found: a directory
that grew past its first block made every path of the volume answer an I/O
error, and a full directory corrupted the tree. The notes are in
[`docs/releases/1.1.1.md`](docs/releases/1.1.1.md); the volume, the syscall
ABI and MOX1 do not change, so a machine of 1.1.0 takes the new image as it
is.

### Fixed

- `mfs`: the new block of a directory that grows is written empty with the
  end of its chain in its header before it is linked, as the growth of a
  file always did. A stale header there made a loop; the lookup followed it
  and the whole volume answered `input/output error` at about the two
  hundredth file of one directory. The regression test fills a directory
  with three hundred files and asks the check for a sound volume; on the old
  code it fails at 172 files.
- `mfs`: a directory that is full answers `no space left on device`
  (`MFS_ENOSPC`) and leaves the volume sound, instead of `E2BIG` and an I/O
  error.
- `kshell`: `umount /` is refused; the root of the tree stays and `mkfs -y`
  formats the volume itself.
- `kshell`: `kill 1` is refused; the init of the machine is not killed from
  the console, and `reboot` is the way to leave.
- `free` prints the memory of the machine and not the uptime; the clock has
  its own command.
- x86-64: the small `printf` of the kernel prints octal, so `stat` says
  `mode: 0644` instead of the literal `0%o`.

### Changed

- `x86-smoke` checks the mode of `stat`, the refusal of `umount /` and that
  `free` has no clock: 165 checks.

## [1.1.0] - 2026-11-22

The release of the board's last major changes: the settings of `/etc`, the
partition of the programs, an update with a rollback, and the first phase of
many cores in the kernel. After `1.1.0` the ESP32 port is a compatibility port
(ADR 0018); the notes are in
[`docs/releases/1.1.md`](docs/releases/1.1.md).

### Added

- The systems of `/etc` (ADR 0021): `/etc/display`, `/etc/font` and
  `/etc/console`, one file of `key=value` lines each, parsed without an
  allocation and applied at the boot - the rotation, the inversion, the theme
  and the scale of the font on the board, the attribute of the VGA cells on
  x86-64. `config get`, `config set`, `config list` and `config sync` read and
  write them, and the store keeps them through a format.
- The layout of the flash of 1.1 (ADR 0022): two application slots of
  1.125 MiB, `progs` of 132 KiB and MicroFS of 5.56 MiB for the user. A board
  of 1.0 loses its volume once, at the move of the partition.
- An update over USB with a rollback: `tools/ota_usb.py` writes the inactive
  slot, the bootloader keeps it in `PENDING_VERIFY` until the shell confirms
  the boot, and a boot that ends before that, or an image whose signature does
  not match, returns the board to the previous slot.
- The programs are files of the `progs` partition, mounted read-only at
  `/bin`; `tools/progs_image.py` builds `progs.bin` at build time and no image
  of a program is embedded in the firmware.
- The first phase of many cores (ADR 0012, `KERNEL_SMP`, off by default): a
  `struct k_cpu` per core, a run queue per core, the scheduler lock as the
  critical section of the port, `k_sched_migrate()` and the rule that a
  running task never moves. With the flag off the kernel, its tests and its
  behaviour are those of 1.0.
- The host port takes the number of cores from `MICRO_OS_CPUS` (1 by default);
  `make test-smp`, `make test-tsan` and `make soak-host` run two of them, and
  the two-core soak runs the thread sanitizer over a million operations.
- The x86-64 port reads the processor list of the machine (ACPI: the root
  pointer, the RSDT/XSDT and the MADT) and remembers the local APIC; the boot
  prints how many processors the firmware describes and how many it runs, and
  `make x86-smoke` boots with `-smp 2` and checks the line. `make
  x86-smp-build` builds the two-core kernel; the APs are not started yet
  (ADR 0012, "The pilot of 1.x").
- The port contract is version 1.9: `kernel/port/cpu.h` with
  `k_port_cpu_count` and `k_port_cpu_id`, and a weak default of one core for a
  port that predates them.

### Changed

- The partition `factory` is replaced by `ota_0` and `ota_1`, and the trailer
  of the signature is the last 4 KiB of the slot that runs (ADR 0020); the
  check of the board reads the running slot, not a fixed address.
- A board of 1.0 moves its volume once: the release notes say to back it up
  and to flash with `--erase-volume` (or to run `mkfs -y`); the NVS survives
  the flash and the update.
- The device test covers `/bin` from `progs` and, with `DEVICE_OTA=1`, an
  update to the other slot, the return and a refused trailer that rolls back.

### Fixed

- `task_signal_interrupts()` reads the mark and the handler of a signal with
  an atomic load, so the reader of another core sees the pair the writer made.
- The tick reads the current task of a core atomically, so the accounting of
  a core no longer races with a switch under `KERNEL_SMP`.

## [1.0.0] - 2026-11-22

The first major release: the contracts are frozen, two machines run the
system, and a session of 600 seconds under a mixed load passes on both of
them and on the host substrate. The gate is
[`docs/releases/1.0.md`](docs/releases/1.0.md); the page of the release is
[`docs/wiki/release-1.0.md`](docs/wiki/release-1.0.md).

### Added

- The syscall ABI, MOX1 version 2 and the MicroFS volume are frozen and
  recorded in `tests/corpus/` with tests that compare the record with the
  running kernel; the contract of the port is version 1.8 and the same test
  holds its list.
- x86-64: the kernel is linked in the higher half (`0xFFFFFFFF80000000`,
  physical `0x01000000`), with a low identity map for the screen and the
  arena; NX and W^X cover the kernel and the programs; `fork` with copy on
  write, ring 3, `EFAULT` at the edge of every call.
- The board runs a `.mox` of Xtensa code without protection: a fixed 8 KiB
  instruction window and a data window, the door of the port as the system
  call; the sleep wakes on the RTC alarm, the factory reset writes the
  defaults of `/etc` back, the IR emitter sends NEC and the sound detector
  listens over the microphone.
- Processes and userland: `pipe`, `dup2`, `kill`, `signal`, `sigreturn`,
  `getlimit`/`setlimit`, a shell with pipelines and redirections, Ctrl+C for
  a job, and ten programs built against `components/libc/user`.
- Filesystems: FAT12/16/32 is read and written, `fsck fat -y` repairs a
  damaged volume, MicroFS repairs its own (`fsck -y`), ext2 is read, and
  `mount`/`umount` and `/etc/fstab` are part of the machine.
- Network: the HTTP client, NTP, MQTT, telnet and the page of the state on
  the board; IPv4/TCP, a server half, `httpd` and a DNS cache on x86-64.
- Signed images: SHA-256 and Ed25519 over a trailer (`MOSIGN01`), the x86-64
  loader verifies the kernel before a byte runs, the board verifies its
  application at the boot, and a build flag decides whether an unsigned or an
  older image is refused or accepted with a warning.
- Quality: reproducible images, 82% of the lines of the kernel in the
  coverage, fuzzing of the ABI and of the parsers, ASan/TSan/valgrind, and
  the three tools of the 600-second campaign (`make soak`, `make soak-host`,
  `make board-soak`).

### Changed

- Programs are files of a MicroFS volume (`make x86-disk`), not blobs of the
  kernel; `/proc`, `/dev`, `/etc` and `/etc/fstab` are the tree of x86-64 as
  they are the tree of the board.
- `execve` carries the descriptors, the working directory and the job of the
  caller; the environment is the third argument of POSIX.
- `ls` lists names (`-l`, `-a`) and `tree` draws the levels.

### Known limitations

- The second core (SMP) and the safe update with a rollback are the first
  projects after 1.0 (ADR 0012, ADR 0011 updated).
- No TLS and no IPv6: 1.0 is a LAN system.
- ext2 is read-only and FAT is written without a journal.
- One user; an object has a mode and an owner, and there is no second party.
- A program of the board has no protection: a bad pointer takes the board
  down, and the documentation says so.
- A pipeline has at most three stages and a task four descriptors.

## [0.8.0] - 2026-11-21

The userland of x86-64 boots from the disk, the kernel and its programs are
W^X, and a `.mox` runs on the board.

### Added

- x86-64 boots its userland from a MicroFS volume on the disk: `make
  x86-disk` writes `/bin` and `/etc` with a host tool, the boot mounts the
  volume and `execve` reads the program as a file; `/proc`, `/dev` and `/etc`
  are part of the machine, and no boot installs a blob.
- MOX1 version 2 carries the machine of the image in the header (`arch`, 0
  x86-64, 1 Xtensa); an image of another machine is `ENOEXEC` before a byte
  runs (ADR 0018).
- ESP32 runs a `.mox`: Xtensa code in an 8 KiB instruction window and data in
  an 8 KiB DRAM window, entered through the door of the port, without an MMU
  and without protection; `/bin/hello`, `/bin/cat` and a foreign image that
  the boot refuses are in the firmware.
- NX/W^X on x86-64: `EFER.NXE`, read-only kernel text and program text, and
  every data page, stack, heap and the screen NX; the port audits its tables
  at the boot and `/bin/nx` proves the faults (ADR 0007).
- ext2, read-only: the superblock, group descriptors, inode table, direct and
  indirect blocks, directories with 255-byte names, and `stat`/`statx` with
  the mode, owner and moments (ADR 0019).
- `fsck fat <point|device> -y` repairs lost clusters in both copies of the
  table, a loop, a cross-link, a broken long name and a size against its
  chain; what the medium cannot explain is named as not repaired.

### Changed

- The port contract is 1.8: `k_port_exec_arch()` names the machine of the
  format, and a port that cannot run a program still names the processor.
- The mode and owner of a file are checked through the path, at `open` and at
  `execve`; NX is not a boot flag.

## [0.7.0] - 2026-11-21

Pipes, signals and a real userland on x86-64; the board's program is a
decision and a plan.

### Added

- `pipe()` (27) and `dup2()` (28): an open file is a counted description and
  the two ends of a pipe are ordinary descriptors (ADR 0014).
- Basic signals: `SIGINT`, `SIGTERM`, `SIGCHLD` and `SIGPIPE`, `kill` (29),
  `signal` (30) and the restorer (31); a handler runs in ring 3 through a
  frame of the port, `waitpid()` is cut by a signal with `EINTR`, Ctrl+C
  reaches the foreground program (ADR 0015).
- `argc`, `argv` and `envp` at the entry, and a library of a program
  (`components/libc/user`) with `_start`, the door, `open`/`read`/`write`,
  `printf` and the strings (ADR 0016).
- `wc`, `head`, `grep`, `mkdir`, `rm` and `pwd` in `/bin`, beside `cat`,
  `echo`, `ls` and `spawn`.
- The shell runs pipelines and redirections (`|`, `>`, `>>`, `<`, `2>`) with a
  task per stage over the inherited descriptors; Ctrl+C reaches every stage
  (ADR 0017). `ls` and `tree` are two commands.
- `fork()` keeps a zombie on the CPU until its exit runs, so a parent that
  waits sees the status it expects.

### Changed

- The wiki site lives on the `wiki` branch; the pages and the link check stay
  in `docs/wiki`.
- The ESP32 program is decided in ADR 0018: MOX1 carries the machine, and the
  board runs a program as a compatibility port after 1.0.

## [0.6.0] - 2026-11-21

The moments of a file, a second writable filesystem, and a network on the
bare machine.

### Added

- MicroFS version 2: 32-byte records with `mtime` and `ctime`, and `statx`
  (26) reports them; a version 1 volume mounts unchanged and reports zero
  (ADR 0013).
- FAT12/16/32 is read and written: create, write, append, truncate, unlink,
  mkdir, long names with their checksum, both copies of the table and
  `statfs`; no journal yet (ADR 0009).
- `mount` and `umount` at runtime over the type registry, several filesystems
  at once, and `/etc/fstab` mounted by the boot after the root (ADR 0009).
- x86-64 network: virtio-net (legacy I/O, polled), ARP, IPv4, ICMP, UDP, a
  TCP client and the server half of the contract, DNS with a cache; `httpd`,
  `http get`, `ntp` and `ping` behind it (`make x86-net-smoke`).
- `fsck fat <point|device>`, read-only: the boot sector and BPB, both copies
  of the table, chains, entries and long names, lost clusters and the free
  count.

## [0.5.0] - 2026-11-21

Protection on x86-64, the first second filesystem, and the gate of 1.0 in the
documents. The 0.5 line was published as `v0.5.0-rc2`; it has no final tag.

### Added

- Ring 3 with page tables of its own, a GDT/TSS and an `int $0x80` gate; a
  fault of a program is a line on the console and the shell survives; `exit`
  of a program returns to its caller; on x86-64 every task has its own errno
  (ADR 0007).
- `fork()` with copy on write on x86-64; the board keeps the placeholder of a
  machine without an MMU.
- `copy_from_user`/`copy_to_user` and `EFAULT` at the edge of a call
  (contract 1.7).
- VGA text and the PS/2 keyboard on x86-64, and a disk on the IDE channel.
- Filesystem types with a registry, and a read-only FAT reader (ADR 0009).
- MQTT 3.1.1 at QoS 0 and 1 over the kernel sockets.
- The board's RTC alarm, deep sleep and a factory reset.
- The optional features behind compile-time flags (ADR 0010), the budget of
  the static DRAM, and a fuzz of the MicroFS parsers.
- The first wiki pages with the link check, and the gate of 1.0
  (`docs/releases/1.0.md`, ADR 0011).

## [0.4.0] - 2026-11-21

Programs as files and the permissions of a file.

### Added

- MOX1, a flat image with a header and a parser that checks it, and
  `execve` (25) with `argv` (ADR 0005, ADR 0006).
- A mode and an owner in the record of a file; `chmod` (23), `chown` (24),
  the `x` bit checked at `execve`, and the path walked with its directories.
- x86-64 runs a program at the boot and from the shell, and a program calls
  the kernel through `int $0x80`.
- `fsck` of MicroFS, read-only: the header, the records and the free map.
- The page of the state over HTTP, and `mkprog.sh` with a check of the
  window a program is linked for.
- The SPM1423 microphone, `PATH` and `stat` in the shell, and the libc
  wrappers of the phase; `execve` answers 0 when the program returns.
