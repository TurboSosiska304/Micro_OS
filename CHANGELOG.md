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

## [2.0.0] - 2026-11-25

### The release

- 2.0.0 "Composition": the machine of its user (`docs/plan-2.0.md`) - the policy
  of the MMU, a `fork` that is a copy of the pages, the group of a record and
  the `setuid` bit, a desktop that starts with the machine, the wire of a
  laptop with DHCP, and a guide a stranger can follow.

### Fixed

- The tables of a firmware above the gigabyte: `k_port_power()` asked the ACPI
  tables for the name of the power block and the walk dereferenced physical
  addresses directly - before 1.9 it worked because the map of the loader
  names the memory of the machine, and after `paging_init()` the identity map
  of the port is the first gigabyte. On a laptop whose FADT lives higher the
  shutdown ended in a page fault, printed "the machine stops here" and left the
  machine on (the clean end itself had done its work: the volumes synced, the
  disks flushed, the mounts gone). The walk goes through a window of two
  megabytes now, the FADT is copied out of it, the S5 port is checked to be a
  plausible ACPI port, and a machine without a FADT says "press the power
  button" and halts - `make power-smoke` is 22 checks with a machine of
  `acpi=off`.
- The Ethernet of a laptop with its window above 4 GiB: a 64-bit BAR is read
  from both halves, a window that answers all ones is not a card, a window the
  port could not map is not registered (the frame path asks whether the map is
  there and drops the frame instead of faulting), and every register access is
  clamped to the 128 KiB of the window. The panic prints CR2 now.
- The reset of a port of xHCI: the power of the port, twenty milliseconds, the
  reset, `PRC` and `PED`, and the change bits cleared after the enable - the
  order Linux uses and the reason "the port 7 is not enabled after its reset"
  was on the screen of a laptop. `make usb-smoke` is 66 checks, with a USB 3
  port among them.

- A guide a stranger can follow: `docs/install.md` - from a stick to a machine
  that keeps a file, with the exact lines the boot prints, a table of
  "symptom -> the line of the log -> what to send", and the recipe of the image
  that has the network. The lane walked its own guide in QEMU and found what a
  guide written by the author cannot: **the stick did not carry `/bin/ports`** -
  the volume of the stick was built by a tool that was never given the SDK
  programs, so `kilo` and the set of sbase were on the disk image of `make
  x86-disk` and on none of the sticks a person writes. `make uefi-stick-smoke`
  is 21 checks with `ls /bin/ports` among them.
### Added

- The accounts of a person, second half (ADR 0044, ADR 0053): `chgrp` is the
  call **51** of the ABI (the count is 52) with an adapter in every filesystem
  (MicroFS, ext2, and `ENOTSUP` on FAT32 which has no group), the group of a
  record is in `struct vfs_stat` with `struct statx` still at 40 bytes, one
  rule of the triads (`vfs_access_bits()`) answers for the VFS and for the edge
  of the calls, `/etc/group` has its parser and `getgrgid`/`getgrnam`, a new
  object takes the gid of its maker, and the image carries the **setuid bit of
  MOX1**: a flag of the header, the real/effective/saved uid of the task, the
  act in `execve` that refuses an image its caller can write, and `pkg` that
  installs such a package only as root. `make x86-smoke` is 429 checks:
  `/bin/suidcat` reads a file of its owner, the same program without the bit is
  refused, a writable setuid image is refused, and the shell keeps its own
  identity after the run.
- `date` is a command of the shell (not a program: the clock is set by a
  privileged act of the kernel and a program could only read it) that prints
  `YYYY-MM-DD HH:MM:SS` and refuses a day that does not exist; the port of
  x86-64 read the year of the CMOS **through BCD twice** - 2026 was printed as
  2020 and 2043 as 2031 - and the date is a gate now.
- The programs we promise are in the image, and a gate says so:
  `tools/x86_image.py --ports` puts the SDK build into `/bin/ports` (kilo and
  the set of sbase, 231 072 bytes), `PATH` of the shell is
  `/bin:/bin/ports:/usr/bin`, and `tests/ports/promised.txt` - one list of
  fifty-seven names, each with the version that promised it - is read by
  `x86-smoke` (four checks: `/bin`, `/bin/ports`, the table of `help` and the
  list of `help`) and by `ports-smoke` (everything in `/bin/ports` is really
  built by the SDK). The owner of a laptop asked for `date` and the machine
  answered "command not found": a promise that nothing checked.
- The stick of a person is a disk, and the memory is not (ADR 0056): the image
  of `make uefi-stick` is a **table of two partitions** - the ESP of 64 MiB with
  the loader, the kernel and a copy of the volume as a spare root, and a second
  partition of **MicroFS** of 24 MiB. The kernel finds that second partition
  over xHCI and its class of mass storage (`usb0p2`), the root mount prefers it
  to the module the loader carries, and the door of the writes opens because
  the medium is ours and `/etc/volumes` names it. The module is read-only and
  **says so** now: the volume the boot carries is memory, and a write to it
  answered `EIO` from the RAM before - the owner of a laptop saw exactly that
  with `write /x.txt hi` and a boot that had said "read-write" of a copy in the
  memory. `make uefi-stick-smoke` is 20 checks: OVMF boots the stick as a USB
  disk, `usb0p2` mounts read-write, a file is written, the power is cut off by
  killing QEMU, and the second boot finds the file with `fsck` clean; a second
  partition of FAT32 is refused and the SHA-256 of the stick does not move.
- The policy of the MMU (ADR 0052): the page tables of a program have rules
  now, and the hardware enforces them. `mmap` of `PROT_WRITE|PROT_EXEC` is
  refused with `EINVAL` **before** a page exists, and the audit of `exec` stays
  as the second lock; the stack grows on the fault of its guard page up to the
  ceiling of the task (`ulimit -m`), with the base read from the tables; `brk`
  and the anonymous mappings are charged against the same ceiling, and the
  heap of the kernel (calls 20-22) is not a program's to take; the kernel is
  unreachable from ring 3 by the supervisor bit on every machine and by
  **SMEP and SMAP** where the processor has them, with a window around each
  copy (`k_port_exec_user_access_begin/end`, the contract of the port **1.14**);
  and a bad access answers `EFAULT` with its address in the log.
  `make mmu-smoke` is 52 checks on a processor with SMEP and SMAP and 51 on one
  without: a program that writes its text, executes its stack, writes a kernel
  address, grows past its heap ceiling or touches a page it does not own ends
  with `EFAULT` and the shell answers the next line.
- The desk of a person (ADR 0054): the machine boots into the desktop - `init`
  starts the session, `/etc/session` runs `/bin/wm` - and the window of the
  **terminal** runs a command of `/bin` on two pipes: the keys go to it, `Ctrl+C`
  is a signal, `Ctrl+T` is the second size of the font (16x32), `Ctrl+Q` leaves.
  The clock of the panel is the time of the machine in the format of
  `/etc/console`, and a second of it repaints **only the rectangle of the
  hour**. `make x86-vga-smoke` is 266 checks, and the lane found a defect of
  1.7 on the way: the damage of a window was compared with the edges of the
  frame instead of the rectangle, so one key repainted 1280x765 - it is 640x16
  now, and two screenshots around a key hold it.
- The wire of a person, first half (ADR 0055): **DHCP** as a client of the
  library (`components/net/src/dhcp.c`): DISCOVER/OFFER/REQUEST/ACK, T1 and T2,
  a NAK that starts again, the lease in `/etc/network` (read before the request,
  written after the answer, `dhcp 0` for the fixed address of 1.8), and a
  fallback that keeps the machine usable when no server answers. And the
  **Ethernet of a laptop**: `e1000e` (the I219 of a ThinkPad and the 82574L of
  QEMU), legacy descriptors, the MAC from the NVM, the window of BAR0 above the
  identity map. `make x86-net-smoke` is 130 checks over three machines: virtio
  with DHCP, `-device e1000e` with DHCP and a download of the host, and a wire
  with no server at all.
- The four defects the owner of a laptop found in the first minute of 1.9.0 on
  a real machine, and the fixes: the **scratchpad buffers** of xHCI (a
  controller of QEMU asks for none, the controller of a laptop for 65, and the
  driver kept 8: no USB at all); the **volume of the boot medium** is
  `boot0p1` and the image did not name it, so the rule of ADR 0041 made the
  stick read-only; a **serial port that is not there** answers 0xFF and the
  console read it as a key, which filled the line of the shell - the keyboard
  of the machine was never asked; and the board of the port called itself
  "QEMU x86-64 machine" on a laptop. The first bytes of the keyboard are in the
  log now.

## [1.9.0] - 2026-11-24

### The release

- 1.9.0 "Composition": the release of the machine that owns its files - of the
  wake that preempts (ADR 0046), of the journal (ADR 0047), of the guard of a
  stack (ADR 0048), of the machine that switches itself off (ADR 0050) and of
  the terminal of a program (ADR 0051, with kilo of somebody else on it)
  (`docs/releases/1.9.md`).
- 1.9.0-rc1: the release of the machine that owns its files - of the wake that
  preempts (ADR 0046), of the journal (ADR 0047), of the guard of a stack
  (ADR 0048), of the machine that switches itself off (ADR 0050) and of the
  terminal of a program (ADR 0051, with kilo of somebody else on it)
  (`docs/releases/1.9.md`).


## [1.8.0] - 2026-11-24

### The release

- 1.8.0: the release of the door of USB, of a disk of a person under the rule
  that a medium this system did not format is read-only, of the library of a
  program nobody rewrote, of a volume whose grain is its own, and of the
  accounts of a session (`docs/releases/1.8.md`).
- 1.8.0-rc3: the packages of the medium are asked for one at a time and a
  missing one is a sentence instead of a failed job (`limine` is not in the
  sources of the container of ESP-IDF, which is what the tag of rc2 found).
- 1.8.0-rc2: the medium of the release is in the bundle of its own tag. The
  step of the CD and of the stick of `release.yml` installed `xorriso`,
  `limine`, `clang` and `lld` **after** the environment of ESP-IDF was
  prepared, and `apt-get` replaced `python3` under it: the build of the
  firmware of the same job lost the module `idf_component_manager` and the
  release of 1.7.0 - and of `1.8.0-rc1` - went out without its ISO and without
  its stick, with a sentence of the step that said so.
- 1.8.0-rc1: the first candidate of the release, without the medium (rc2).

### Added

- The guard of a stack (ADR 0048): the stack of every task of the kernel has a
  page under it that is not mapped, on x86-64 and on the host, so a store past
  the bottom of a stack is a fault **of that task** and not a field of the heap
  of its neighbour found much later somewhere else (the 16 KiB stack of the
  shell under `pkg -S` faulted in `k_sched_tick`). The handler recognises the
  address and ends the task; the machine answers the next line, `/bin/crash
  stack` is the program that proves it, and `tests/cases/test_guard.c` holds
  the same contract on the host - with an alternate signal stack and a
  `siglongjmp`, which is the only way a thread of the host leaves a stack that
  is gone. The audit of `exec` refuses an image that asks for a page both
  writable and executable, and names the segment it refused. `make x86-smoke`
  is 412 checks.
- A wake that preempts (ADR 0046): the one defect the ABI of the kernel had
  carried since 1.0 - `k_wait_wake` moved `wake_tick` to the present and the
  waiter became runnable at the next tick, so a round trip of a pipe was
  **1.006 ms**, one tick, on the machine and on the host. The waiter **arms**
  its record under the lock of the object, looks, and then sleeps
  (`k_wait_arm`/`k_wait_sleep`/`k_wait_disarm`), so the window of K15 - a wake
  that arrived between the look and the registration and was lost - is closed
  by construction; a wake runs the queue of the core (or sends the IPI of K8),
  and the port's sleep is cut by the wake itself. `lat_pipe_p50` of x86-64 in
  QEMU is **19 968 ns** and p99 46 592 ns (from 1 005 056 and 1 044 992), and
  the host answers a pipe round trip in **3 µs**. The twenty-millisecond chunk
  that 1.8 put on the waits of a pipe and of `poll` is gone.
- A terminal of a program (ADR 0051): `/dev/tty` with `termios` - `tcgetattr`,
  `tcsetattr`, `cfmakeraw`, `VMIN`/`VTIME`, `TCGETS`/`TCSETS`, `TIOCGWINSZ`
  and an `isatty` that asks - the screen of the console as a property of the
  port, and a console that understands the cursor and the clearing of a
  screen. On it, **kilo**, a full-screen editor of somebody else (BSD-2, not
  one line changed), is built by the SDK and driven on the machine:
  `make tty-smoke` opens a file, types a line, saves with `Ctrl-S`, quits with
  `Ctrl-Q` (the keys of antirez kilo, not of a guess) and reads the line back
  out of the volume; `make ports-smoke` is 73 checks. The descriptors 0, 1 and
  2 of a task are its terminal while nothing is opened on them, so a program
  finds the console without opening `/dev/tty`; the mode belongs to the
  terminal and the console goes back to canonical when the program that
  changed it goes away; the console of the port understands the cursor, the
  clearing and a **deferred wrap** at the last cell (the status bar of kilo,
  exactly as wide as the screen, scrolled the frame once per redraw until that
  was found).
- A machine that switches itself off (ADR 0050): `shutdown`, `poweroff` and
  `reboot` are commands of the shell and `k_power_shutdown` is the order of a
  clean end - the sessions are closed to new ones, the volumes are synced and
  the disks flushed (`FLUSH CACHE EXT`, the NVMe `Flush`) **before** any task
  is signalled, then `SIGTERM`, a second sync for what the tasks wrote, the
  unmount of every volume and the power. A flush that fails refuses the
  shutdown and the machine keeps working (ADR 0041: the data first). The port
  grew `k_port_power()` (contract 1.13): ACPI `S5` through the FADT on x86-64,
  with the ports of QEMU and Bochs and the reset register behind it; deep sleep
  on the board, with `HOLD` kept and the button of the board as the wake; a
  documented exit code on the host. `make power-smoke` proves it: the process
  of QEMU **ends by itself** on `shutdown`, the same image boots again and its
  volume is clean, and `reboot` boots the same process a second time.
- The accounts of a person, second half (ADR 0044): a **session** is a task
  with a terminal of its own (`kernel/console.h`), `init` of the console is a
  getty that starts one and starts another after a `logout`, `who` and
  `logout` are commands, and `/bin/login` is not needed - the console is the
  person in front of the machine (root, no password, the rule of the ADR),
  while `login` and `su` are the door for another account. The dialog of the
  door is the same code for the console and for the wire: three attempts, one
  sentence for a wrong password and for a name that is not there, a delay, and
  a hash computed for both so that the two take the same time. The owner of a
  new file is the uid of its maker (`vfs_open(O_CREAT)` and `vfs_mkdir` ask
  the filesystem for a `chown`), so a file a guest writes is the guest's -
  `x86-smoke` reads `owner: 7` where it was `owner: 0`.

- A machine of a laptop keeps its files: the write path of the two disks of
  M2, behind the rule of ADR 0041 and not in front of it. The root mount asks
  for the writes after it is in the tree (`vfs_mount_root_writes`), so its own
  `/etc/volumes` can be read; another volume asks with
  `vfs_mount_type_write()` or the `rw` option of `/etc/fstab`; a volume the
  rule does not name is mounted read-only with the reason in the log. The
  drivers grew `WRITE DMA EXT`/`FLUSH CACHE EXT` and the NVMe `WRITE`/`Flush`
  of the namespace, the image tool names the volumes it formats, and
  `make x86-disk-smoke` proves a file that survives a reset on the IDE channel,
  the SATA controller and the NVMe namespace - while a volume the list does not
  name and a volume of btrfs leave the SHA-256 of their image unchanged.

- The door of a laptop, second iteration: a **hub** of the class 09 (the power
  of its ports, the reset of one, the route string of a device behind it, four
  tiers of eight ports), so a keyboard or a stick behind a dock is found; the
  **hot-plug** of the root ports and of the hub ports, so a device inserted
  after the boot is enumerated and one taken out leaves the input, the block
  layer and the network without a panic; a **mouse** of the HID boot protocol
  as the pointer of `/dev/input0` beside the PS/2 one (the wheel is a key of
  the event contract, added to it and not taken from it); and the **line** of
  the controller, MSI-X first with MSI and the legacy pin behind it, in place
  of a poll of the event ring. The event ring is one consumer for the three
  classes now - a completion of a bulk transfer of the network is not thrown
  away by the pump of the console - which the wire of `usb-smoke` had been
  losing. `make usb-smoke` is 58 checks.

- The door of a laptop, USB (ADR 0040): `ports/port_x86_64/src/xhci.c` finds
  the controller of the bus (class 0x0C/0x03), resets it and programs it - a
  command ring and an event ring with its ERST, the slots and the contexts of
  a device, the ports with their speed and their reset - all of it by physical
  addresses of the port's contract (ADR 0032). On it: a keyboard of the HID
  boot protocol whose eight-byte reports the portable kernel decodes
  (`kernel/input_hid.c`, `tests/cases/test_input_hid.c`), with the same events
  and the same characters as the PS/2 one, so a machine with both reads them
  as one keyboard; a stick of mass storage (BOT, class 08/06/0x50, `INQUIRY`,
  `READ CAPACITY(10)`, `READ(10)`, `WRITE(10)`) registered as `usb0` of the
  block layer and the rule of ADR 0041 over it; and CDC-ECM as the second
  network device of the port (`usbnet0`, frames over two bulk endpoints, the
  MAC from the descriptor of a string). `make usb-smoke` runs a machine with
  `i8042=off` - no PS/2 at all, so the keys can only come through xHCI - and
  `make usb-stick-smoke USB_STICK=/dev/sdc` boots with the stick of a person
  attached read-only: 35 and 43 checks.

- The grain of a volume (ADR 0043): the sector is a property of the volume,
  not of the medium. `struct k_blockdev_view` is a device of the block layer
  whose sector is the one of the superblock, over a device whose sector is
  smaller - arithmetic over one call, no copy and no allocation - and ext2
  opens one when its superblock says a block of 4 KiB on a medium of 512-byte
  sectors. `e2fsck -fn` and `debugfs` are clean on every path of a write at
  that grain (a file past the twelve direct blocks, a hole past the single
  table, a truncation inside a block, a truncation that gives the table back,
  a growth into a hole), and `make x86-volume-smoke` writes and reads a volume
  of 5 GiB at both grains: 32 checks.

- The library of a program nobody rewrote: `popen`, `pclose` and `system` are
  a pipe, a fork, a dup2 and the execve of the first word of the line, and
  `make ports-smoke` builds **thirteen utilities of sbase with not one line
  changed** - `true false echo basename dirname pwd printenv sleep cksum tee
  cmp whoami sha256sum` - with the toolchain of `make sdk`, runs them on the
  image of the release and checks their answers: 62 checks. `tests/ports/
  upstream.txt` holds the digest of every vendored file and the gate
  recalculates it before the machine starts, so a byte of a local edit fails
  the gate; `docs/ports-table.md` is the table of the lines of source, of the
  lines of a port layer and of the patches, per program (577 / 0 / 0 for the
  thirteen).

- The disk of a chipset, on the SATA controller of a laptop (ADR 0041):
  `ports/port_x86_64/src/ahci.c` speaks class 01/06 of the PCI bus with its
  window of registers above the identity map (CAP/GHC/IS/PI, a command list
  and a FIS area per port, a PRDT of physical pages), reads the identify data
  of the drive and the table of its partitions (MBR and GPT, the parser of the
  kernel), mounts the volume of the first partition as the root of the machine
  and runs `/bin/hello` out of it. The disk is **read-only**: the rule of
  `components/kernel/src/volume.c` refuses every write of a volume this system
  did not format and `/etc/volumes` does not name, a volume of another system
  (btrfs, ext4, NTFS, XFS) is refused at the mount, and the table of the
  partitions is never written. `make ahci-smoke` boots the image on a q35
  machine and proves it twice by the SHA-256 of the whole image before and
  after the boot - once for the volume of this system, once for the volume of
  btrfs.

- The disk of a modern laptop, on the NVMe controller of the PCI bus
  (ADR 0042): `ports/port_x86_64/src/nvme.c` enables the controller (AQA, ASQ,
  ACQ, CC.EN and CSTS.RDY), identifies the controller and the namespace 1,
  creates one pair of I/O queues through the admin queue, and reads the
  namespace with `PRP1`/`PRP2` - the two physical pages of a request, by the
  translation of ADR 0032 - in commands of at most 8 KiB. The namespace of 512
  bytes is the sector of the medium, the block layer sees the kilobyte of
  MicroFS, and the disk is read-only by the rule of ADR 0041: there is no
  `WRITE` command in the driver, and `make nvme-smoke` proves a mount, a
  program and the SHA-256 of the whole image before and after the boot - once
  for the volume of this system and once for a volume of btrfs.

### Fixed

- A sleep could answer before its deadline: the port's sleep is a door that a
  wake of another channel knocks on too, and `k_sched_sleep_ms()` took the
  first answer - so a session of three wrong passwords waited less than three
  times the delay it was told, and a case of the terminal measured 90 ms of a
  sleep of 100. The sleep is a loop on a deadline taken once at the entry now,
  and only the kernel ends it (the tick, a wake of this task, a signal, the
  stop flag of a wait). `make test-valgrind` was the gate that found it, and
  `tests/cases/test_wait.c` holds it: a task sleeps 50 ms while another wakes
  its door every 2 ms, and the case answers 52 (0 before the fix).
- A directory entry carried a byte nobody wrote: `dir_find` filled the name,
  the record and the type of a `struct mfs_dirent` and left `reserved` as the
  stack had it, `dir_add` copied the eight bytes of the header whole, and the
  byte went to the medium, into the journal slot and into its checksum -
  valgrind of the cloud named the rename of the host case. The header is built
  from a zeroed struct now, `dir_find` and `dir_entries` zero what they hand
  out, and a case fills the stack with `0x5A`, renames, reads the block of the
  directory back from the medium and asks the reserved byte to be zero.
- `telnet` asked for the password on the console of the machine and not on the
  socket of the session: a client of the wire could not log in at all, and the
  person in front of the board was asked for a password that came from
  somebody else. It is the defect of 1.6 that the cleanroom wrote down. A task
  has a terminal now, the session of a client puts the socket there, and the
  echo of the password is agreed with the client (RFC 857) and never put in
  the transcript - `x86-net-smoke` checks that `password: ` never appears on
  COM1, which is the proof that the dialog went to the wire.

- A child of a `fork` took its fields from the parent **after** its slot was
  in the table and marked in use, so the child - a thread of its own on the
  host and a task of another core in `KERNEL_SMP` - read a record that was
  still being written: the race detector of the cloud named the read of
  `task_exit` against the write of `task_spawn`. The inheritance happens in
  the critical section of the slot now, and the table of the descriptors,
  which has the lock of its own in the opposite order, is copied after it and
  before the task can run.

- The suite under valgrind, the gate of the cloud, was red for two days
  without a single failed check: a checksum over the padding of a record
  (`tests/cases/test_mfs_format.c`) and a `strstr` over the bytes a file of
  `/proc` did not write (`tests/cases/test_sysfs.c`) read uninitialised
  memory, and a child of a `fork` inherits the count of its parent, so its
  exit code became the 42 of `--error-exitcode` and a case that read the exit
  code of its program read 42 instead of the answer. Both reads are fixed and
  `make test-valgrind` runs the same command here as the cloud does: 0 errors
  from 0 contexts, 76 058 checks.

- A wait of a pipe or of `poll` could sleep for as long as the machine was up:
  `k_wait_block` registers the waiter after the caller has looked at the
  object, so a wake that arrived in that window found no record and was lost,
  and a wait without a deadline never looked again. A pipeline of the shell
  never finished under a slow runner of the cloud - the test suite died on its
  timeout with no failed check, which is what a hang looks like. A wait is
  twenty milliseconds long now (`KFILE_WAIT_CHUNK_MS`), which is the chunk the
  sleep already had, so a lost wake costs a turn of the loop and not the line;
  the cause and its fix are K15 of `docs/debts.md`.

- The read ahead of the block layer (ADR 0032) armed the wait of the stream
  *inside* the call that read, so a write of 16 MiB at a grain of 4 KiB read
  36 MiB of the disk before it wrote a byte. The wait belongs to the stream and
  not to the call: the same write reads 2.5 MiB, and the write of the same file
  went from 129 to 184 MiB/s. Held by a case of `test_blockdev_cache.c`.

- The descriptors 1 and 2 were "the console" only while nothing had opened
  them, so the first `pipe()` or `open()` of a program took them: `tee` wrote
  its answer into its own file instead of the console, and any program that
  spoke after an `open` spoke to the wrong place. `open` and `pipe` move a new
  descriptor off them now (the probe is `fstat`, not `read`: a read of the
  write end of a pipe answers `EBADF` too), and an explicit `dup2(fd, 1)` is
  still obeyed.

- A case of `tests/cases/test_ksys_process.c` waited three seconds for a
  program it had started as a task among the others: a host that runs a build,
  a sanitizer suite and three copies of the case answered a failure of the
  property instead of a slow machine. The waits of that file are ten seconds,
  and the status of the `fork` case is under the same condition as its use
  (the race detector refused to compile the file, so `make test-tsan` was red
  and nobody had looked).

## [1.7.0] - 2026-11-24

### The release

- 1.7.0-rc1: the release of the desktop, of the disk at the speed of the
  medium, of a filesystem that does not walk a table of a gigabyte, and of a
  machine that starts on a laptop from a stick (`docs/releases/1.7.md`).


### The release

- 1.6.0-rc1: the release of the memory a program maps, the account of a
  task, the interrupts of many cores, the disk on the bus, the pipes at the
  speed of the memory and the machine in the network (`docs/releases/1.6.md`).
- 1.6.0-rc2: the 600-second session of the tag refused to build its image -
  the programs of the release no longer fit the 8 MiB of that volume
  (`mkfs: /bin/sqlite3 took -4 byte(s) of 848800`). The volume of the session
  is 24 MiB and the filler of free space steps down by 256 KiB, because a
  single file near the free count of the volume is refused by the chain and
  the table and not by the count.


### Added

- The palette of the desktop (`components/ui/include/ui/palette.h`): the
  server of `/bin/wm` and the programs of the interface paint the roles of a
  theme - a slate desk with an azure accent, or the light theme of
  `theme=light` of `/etc/console` - and not the sixteen colours of the CGA.
  Eight bits a channel narrowed to the five-six-five of the screen once; a
  gradient and a grid for the wallpaper, a drop shadow and a ring of the
  accent for a window, a separator and two inks of hierarchy in its frame, air
  around the text, and a pointer of white with a black outline. The console of
  the kernel keeps its sixteen attributes.
- The boot of a firmware machine: `ports/port_x86_64/loader/uefi`, a PE/COFF
  application that the firmware of a laptop starts from a FAT volume, gives
  the kernel the picture of its graphics output (a frame of 32 bits per pixel,
  and a shadow of the console in RGB565 converted a cell at a time,
  `src/gop.c`), the map of its memory in the shape the port reads, the root of
  its ACPI tables and a `disk.img` read out of the same volume and mounted
  from the memory - so the machine boots with no driver for the controller the
  volume is on. `make uefi` writes the volume (`BOOTX64.EFI`, the kernel and
  the disk image) and `make uefi-smoke` boots it under OVMF and checks the
  loader, the kernel, the shell and the picture of the card (ADR 0037,
  `docs/ports/x86_64/uefi.md`). The kernel of the BIOS path is untouched: one
  kernel, two loaders.
- A server of the files of the volume, `httpd serve [port] [root]`
  (ADR 0036): HTTP/1.1 with keep-alive, GET and HEAD, `Content-Length`, the
  type from the extension, `ETag` and `304`, one `Range` with `206` and
  `416`, a listing of a directory as a chunked page of links, a `301` for a
  directory named without its slash, a line of `/var/log/httpd.log` per
  request, an idle timeout, a ceiling of the sessions that answers `503`, and
  a path that is decoded and walked under the root, where a `..` that would
  climb out is `403`.
- The serving side of TLS, `httpd tls [port] [root]` on 443: the vendored
  mbedTLS with `MBEDTLS_SSL_SRV_C`, the certificate and the key of
  `/etc/ssl/server.crt` and `server.key` through the VFS, and `curl -k`
  against the x86-64 guest in `make x86-net-smoke`. The board has no TLS on
  either side and says so.
- An update over the network, `ota fetch <url>` (ADR 0022 §7, ADR 0036): the
  signed image is downloaded into a staging file, the trailer and the
  Ed25519 signature are checked **before** the inactive slot is written, the
  slot is selected and the reboot is the caller's; a slot that never
  confirms itself is rolled back - in the file form of x86-64 and in otadata
  on the board. `ota status`, `ota boot` and `ota confirm` are the state at
  the console, and `wget --sha256` reports the digest of what it read.
- The system call of the x86-64 port takes the fast door of the processor
  (ADR 0035): the library of a program calls with `syscall` and returns with
  `sysret`, the port programs `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR` and
  `IA32_FMASK` on every core, and the entry finds the kernel stack of the
  program in the per-core record under `IA32_GS_BASE` - the same number
  `port_x86_64_tss_set_rsp0()` writes into `RSP0` of the TSS. The frame is the
  same 160 bytes in the same order as the vector 0x80 leaves, so the signal
  frame, the child of a `fork()` and the replacement frame of an `execve` did
  not move; `sysret` is guarded (a canonical address and a ring 3 selector, or
  the frame goes back through `iretq`), because the frame of a `sigreturn`
  comes from memory the program can write. The vector 0x80 stays as the second
  door: it is what a program of another compiler calls and what the restorer
  of a signal handler returns through, and it now leaves through the same fast
  tail. A `getpid()` of a program costs 0.55 µs instead of 0.92 µs on the QEMU
  of the tests - 1.8 million calls a second instead of 1.1 - and a `grep`,
  `sed` or `wc` of a file of 1 MiB moves by less than the noise, because the
  door is not what those commands spend their time in.
- The grain of the I/O (ADR 0035): the ring of a pipe is a setting of the
  port - 64 KiB on x86-64 and the host, the block of one call of the system,
  4 KiB on the board - and comes from the allocator with the pipe, so sixteen
  pipes of 64 KiB cost a megabyte only when they are all open and the board
  pays 4 KiB per stage; a write of `PIPE_BUF` bytes waits for the whole block
  and lands whole, and the wait of a pipe has no bound - a reader slower than
  its writer sleeps instead of answering `EAGAIN`, and a signal cuts the wait
  short with `EINTR`. A stream has a real block from the arena of the program
  and `setvbuf` works: `_IOLBF` on a terminal, `_IOFBF` on a file and a pipe
  (4 KiB), `_IONBF` for the error descriptor, `fflush`, `fclose` and `exit`
  over it, and `printf` reaches the block of `stdout` through a weak seam
  when the program links the streams. `/bin/streams` checks the three modes
  and the flush of `exit`; `/bin/iobench` and `tools/io_bench.py` are the
  ruler of the machine. One megabyte through `cat file | wc -c` went from
  5.9 s to 0.47 s, the same megabyte through `iobench pipe` from 0.20 to
  29 MiB/s with the reader's calls down from 4096 to 16, and a filter of
  sbase over a megabyte of text is 1.2-1.5 times faster (its time is the
  regular expression, not the calls).
- The accounts of the machine are files of `/etc` (ADR 0033): `/etc/passwd`
  holds `name:uid:gid:home:shell` and `/etc/shadow` the salted SHA-256 of a
  password (`$sha256$<salt>$<hex>`, `*` for an account without one, mode
  `0600`), `getpwnam` and `getpwuid` read the same file as the shell, and
  `login`, `su` and `id` are commands of the console: `login` asks the name
  and the password three times and hands the shell to that user, `su` lets
  root become anybody without a password and a user become another only with
  the password of the account, `-l` enters the home, and `id` says who the
  shell is. `task_set_creds` moves the gid and the uid of a task in one
  critical section. There is no setuid bit in `MOX1` and no gid in the record
  of a filesystem: the console is the door, and both are a format change with
  an ADR of their own. `make x86-smoke` boots the machine, logs in as root
  with a wrong and then the right password, becomes uid 7 with `su -`, refuses
  `su root` with the wrong password, and reads the owner of a file the user
  wrote; 340 checks.
- The disk of the x86-64 port speaks bus-master DMA (ADR 0032): the PCI IDE
  function is found by its class, the bus-master bit is set in its command
  register, a PRDT of 4 KiB pieces (no entry crosses a page) moves up to
  64 KiB in one command, and the transfer ends with IRQ 14 instead of a poll
  of the status register. A machine without the function, a drive whose
  identify data says it cannot and a buffer whose pages the port cannot
  translate all keep the PIO path, which is unchanged.
- ADR 0032 states the contract of a physical address: which addresses may
  reach a device, who translates them (`port_x86_64_bus_phys` for the heap
  and the image, `port_x86_64_paging_phys` for a page of a program), what
  keeps the memory alive while the transfer runs, and why x86-64 needs no
  cache maintenance around a DMA.
- `/proc/blockdev` lists the block devices with the state of the cache above
  each of them: the bytes of a sector, the sectors of the device, the boxes,
  the boxes that hold a sector, the dirty sectors the medium does not have
  yet, and whether the cache writes back.
- `k_blockdev_register_cached_sized()` and `k_blockdev_cache_stats()`: a
  device asks for a cache of its own size (the disk of the x86-64 port asks
  for 512 sectors, half a megabyte, because a bus master moves a run at a
  time) and a reader can ask what it holds.
- A task carries a uid and a gid, and the triads of a file are read against
  them: `getuid`, `geteuid`, `getgid`, `setuid` and `setgid` take the numbers
  36 to 40, root passes every triad while anyone else gets the bits of the
  owner or of "others" (no filesystem of this system stores a group), `chmod`
  is the owner's or root's, `chown`, `mount` and `umount` are root's, and
  `kill` of a task of another uid answers `EPERM`. The credentials are
  inherited by `task_create` and by `fork`, and `execve` keeps them.
- `rename` reaches ext2, and `rmdir` is a call of its own (41) in the VFS and
  in MicroFS, FAT32 and ext2: `unlink` of a directory is `KERR_ISDIR`, `rmdir`
  of a file is `KERR_NOTDIR`, a full directory is `KERR_NOTEMPTY`, and
  `rm -r` and `/bin/rmdir` use the call.
- `fstat` (42) answers the mode, the owner, the size and the two moments of
  the object behind an open descriptor, where the library used to answer 0666
  and the size through `lseek`; `O_EXCL` is a flag of `open` and of the ABI.
- `ext2_repair` and `fsck ext2 [-y] <point>`: the counters of the superblock
  and of every group, both bitmaps, the link counts, the `.` and `..` entries
  and the size of a directory against its chain, and a lost file reconnected
  as `#<inode>` under `/lost+found`. The oracle is the host: `e2fsck -fn`
  answered "Free blocks count wrong" with exit 4 before the repair and exit 0
  after it.
- The map of `mfs_check` is sized by the volume instead of a constant of 32 768
  records, so a volume of 40 000 records repairs an orphan past the old
  ceiling.
- SQLite keeps its journal in a file (`journal_mode=DELETE`): `make
  x86-fs-smoke` cuts a transaction with a reset and reads the rolled-back rows
  on the next boot, `PRAGMA integrity_check` answering `ok`.
- The switch of a task saves and loads the XMM and x87 state (512 bytes of
  `struct port_task`, `fxsave64` and `fxrstor64` in `commit_switch()`), a
  `fork` copies the live state into the child, `execve` starts the new image
  from a clean template, and the trampoline of an application processor turns
  `CR4.OSFXSR`/`OSXMMEXCPT` and `CR0.NE` on - without them SSE in ring 3 of
  the second core was `#UD`. `/bin/fpu2` is the test that failed before it:
  two children compute in `xmm0`/`xmm1` while the parent computes, and a third
  dirties x87 and the control words before an `execve`.

- A program has the POSIX process pair on x86-64: the child of `fork()` is a
  task with a copy-on-write image, `execve()` from a program replaces the
  image of the caller instead of starting a second task (the pid, the parent,
  the descriptors and the directory stay), and `waitpid()` reaps the child and
  answers its status, with `-1` meaning "any child". `/bin/procs` proves the
  four shapes in `make x86-smoke`: a child that computes 55 and returns it, a
  double fork with two statuses, and a child whose `execvp` becomes another
  program. `make x86-smoke` is 319 checks. The shell stays the synchronous
  caller it was, because a kernel task has no address space to replace.
- A pipeline of the shell is no longer three stages: the parser holds sixteen
  (the most a line of 32 words can carry), the pipes are made one boundary at
  a time as the stages are forked - the shell keeps one reading end between
  two forks and closes the boundary behind it - and the system has sixteen
  pipes and forty open-file descriptions. A line of ten stages and Ctrl+C for
  the whole job are in `make x86-smoke`.
- `xargs -P` works: the command of sbase forks a child per batch, the child's
  `execve` replaces it, and the parent reaps with `waitpid(-1)` without
  blocking below the ceiling of `-P`. `make x86-smoke` runs six commands with
  `-P 4` and checks that each word was handed to one child.
- The interface has programs (ADR 0030): `/bin/edit` is a text editor on the
  screen - a file, the arrows, the characters of the keyboard, `Ctrl+S` to
  save and `Ctrl+Q` to leave, a title bar, a status line and a modal window
  that asks about an unsaved file - and `/bin/fm` is a file manager: the
  directory through `readdir`, the selection, Enter to open a directory or
  hand a file to `/bin/edit`, `d`/Delete to `unlink` after a modal question
  and `q` to return. Both write only the rows they changed. `make
  x86-vga-smoke` boots a copy of the disk, types two lines into the editor
  with `sendkey`, saves, reboots and reads the file back with `cat`, and
  lists, moves and deletes in the file manager; 129 checks.
- The window model, the canvas and the keyboard are `components/ui`, portable
  and tested on the host (`tests/cases/test_ui_windows.c`): a window is a
  rectangle in the order of the paint with a focus and a modal flag, the
  canvas fills, outlines and lays out a line of cells of a font the caller
  gives, and the keyboard maps the names of `/dev/input0` to the characters
  of the US layout. `/bin/ui` uses the model for its two windows.
- The arrows, Home, End, PageUp, PageDown, Insert and Delete are keys of the
  machine: the E0 prefix of the scancode set 1 is translated by the portable
  table `kernel/input_set1.h` instead of being dropped, the ten numbers are
  appended to `enum k_input_key` (69-78) and never move, `key.up` and
  `key.delete` are events of `/dev/input0`, the right Ctrl is a modifier of
  the same form, and the Enter and the slash of the keypad still type their
  character for the shell. `tests/cases/test_ui.c` holds the numbers and the
  table.
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
- The timer of the PIT and the keyboard of the x86-64 machine are lines of the
  I/O APIC and no longer of the 8259: the port reads the table of the firmware
  (the address of the chip and the ISA overrides of the MADT), programs one
  redirection entry per line - the vector, the destination core, the polarity,
  the trigger and the mask - masks the legacy chip as a whole and sends the EOI
  to the local APIC of the core that took the line. A machine whose firmware
  describes no chip keeps the 8259, and `smp irq` prints the table;
  `smp irq keyboard <cpu>` moves a line to another core, which is how
  `make x86-smp-smoke` sees the keyboard arrive where it was sent (ADR 0034).
- A task that the scheduler puts in the queue of another core wakes that core
  with an inter-processor interrupt instead of waiting for its next tick: the
  port contract gains `k_port_cpu_wake()` (weak in `src/cpu_default.c`, so a
  port with one core keeps the tick), the scheduler calls it after it drops
  its lock, and the x86-64 port sends the vector `0x51`, whose body claims and
  switches. The port counts the tasks that began before the tick of the core
  and the ones that began at it, and `smp wake` stops the clock of the other
  core to show that an interrupt - and nothing else - starts a task there
  (ADR 0034). `make x86-smp-smoke` is 52 checks.
- The image of `make x86-disk` carries the two files an `https://` repository
  needs on a fresh machine: the bundle of roots of the Mozilla program in
  `/etc/ssl/certs.pem` (`tools/ca/README.md`) and the key of the repository of
  the project in `/etc/pkg/keys/microos-progs.pub` (`tools/pkg/README.md`).
  The documented `pkg repo add` + `pkg -Sy` + `pkg -S hello` installs a
  package out of the box, and `make x86-net-smoke` checks it on the image of
  the release: 59 checks.

### Fixed

- The keyboard of a laptop whose firmware leaves the translation of the 8042
  off: it speaks the scancode set 2, and the port read every byte of it as the
  set 1 - the prompt arrived, the keys did not type their letters, and a line
  of rubbish filled the buffer until "line too long, it was not executed". The
  boot now asks the keyboard which set it speaks (`0xF0 0x00`) and the
  controller whether it translates (bit 6 of its configuration), the line
  `ps2: the keyboard speaks set N` says what the machine answered, and the
  translation of the set 2 and of the set 3 is a table of the kernel
  (`kernel/input_scancode.c`) with the decoder of the make, the break, the
  prefix of the extended block and the sequence of the Pause - the same keys of
  the machine out of every set, which the host test of `tests/cases/test_ps2.c`
  holds key by key in the three. The keyboard of QEMU (a device of the set 2
  behind a controller that translates) and of every PC whose firmware predates
  the question is the set 1, exactly as it was.
- The boot of a medium whose loader puts a module inside the window of the
  programs. limine, in the EFI half of the hybrid ISO, leaves its modules right
  after the kernel file at 23.5 MiB, so a root image of 16 MB runs from 23.5 to
  39.5 MiB - through the 32-48 MiB the port maps onto the arena of the
  programs. The kernel put its heap at 35.5 MiB, that is inside the window, and
  the first program to run read the heap as a program's image: `#PF` at the
  stack, then the storm. The heap is now placed in a piece of the memory that
  is outside the window and the arena, and a module that a loader left inside
  the window is copied out of it - above the arena, before the page tables of
  the port exist - with the entry of the module table written to the new
  address. The BIOS path had the other half of the same question: with a root
  image of 16 MB, limine's module crosses `KERNEL_PHYS` at 16 MiB and
  `loader32.S` refused the boot; the loader now takes the first free 2 MiB
  block at or above 16 MiB and hands that address to the kernel in the third
  argument of the entry, so a medium of any size boots. `make x86-iso-smoke`
  is 14 checks on a 16 MB root image (it was red with limine on this machine),
  and the ISO of a 16 MB root boots both ways in a hand-built hybrid image.
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

- `waitpid(-1)` and `wait()` could not wait for any child: the pid arrives in
  an unsigned register, where -1 is larger than every pid, and the scan of the
  children compared it as a number - the wait answered `ECHILD` and `xargs`
  could not reap the children it had forked. The scan compares the signed
  value, and a host test waits for `-1`.
- The parent's first write after a `fork()` went into the frame the child was
  about to return into: the clone made both page tables read-only but flushed
  the TLB with an address 32 MiB above the page (the base of the window was
  counted twice), so the stale writable entry stayed. The clone flushes the
  address it changed, and a libc program's child returns 0 without the
  library's `getpid()` workaround and its yield.

- A pipeline lost the output of its last stages: the shell keeps no
  descriptor of its own, so `pipe()` handed out the low numbers and a read
  end of a boundary could land on descriptor 1 - the standard output of the
  stage, which then answered `bad file descriptor` (`15 stages` of
  `cat | ... | wc -l` printed no count). `stage_setup()` moves the two ends
  above 2 and closes the three before the copies, so a stage owns exactly 0
  and 1 whatever the numbers of the pipes were.
- The documented maximum of a chain did not run: sixteen stages of programs
  need 34 tasks of a table of 32 (a stage is two tasks, the stage and the
  program) and the last ones were never started, silently. The shell counts
  the task slots before it makes the first pipe and refuses a chain that does
  not fit with the numbers; fifteen stages of programs fit on x86-64 and
  sixteen of the commands of the shell, and `docs/wiki/shell.md` says why.
- `pkg` could not download anything over `https` on an image out of the box:
  `/etc/ssl/certs.pem` was not there, a certificate signed with
  ecdsa-with-SHA384 was `signature algorithm (oid) is unsupported` because
  SHA-384 is a module of its own in mbedTLS 3.6 (ISRG Root X2 and the P-384
  chain of Let's Encrypt were unreadable), and one certificate the image does
  not implement made the library refuse the whole bundle. SHA-384 is enabled,
  a bundle is loaded whole with the certificates that cannot be parsed skipped
  and counted once in the log, the head of an answer is read up to 1024 bytes
  (GitHub Pages answers 702, and the old 640 said "the address or the answer
  is too long"), and `pkg` names the key file when a signature does not match.
- `kill -TERM <pid of the shell>` left the machine without a console until a
  reset: the console is the only shell and the port has no `getty`
  (ADR 0033). The signal to the task that runs the command is refused with a
  sentence, as the one to `init` already was.
- A name longer than a volume keeps was `file too big` or `invalid argument`:
  `mkdir`, `write`, `rm`, `stat`, `ls`, `tree` and `df` of the shell say
  `file name too long`, the limit of MicroFS (32 bytes) named as the limit it
  is.
- The `mkdir` of the shell took one directory and dropped the other words of
  the line in silence; every word is a directory now, as in Linux and in the
  program of `/bin`.
- `df` printed `(0 KB each)` for the 512-byte blocks of a FAT32 volume and
  `inodes: 0 of 0 free` for a volume that keeps no inodes. The unit of the
  block is named (`512 bytes each`, or `1 KB each`), and a volume without
  inodes says `not kept by this volume`.
- `login` with no name said `the name does not fit` and then `no such
  account`; it says `a name is needed`, and nothing else.
- The help of `cat` promised `-n` without saying that on x86-64 the `/bin/cat`
  of sbase runs at the prompt and takes `-u`.

## [1.6.1] - 2026-11-24

A patch of `1.6.0` for one defect of the library, found by the lane that gave
`setenv` and `fscanf` to it.

### Fixed

- `strtoll` and `strtol` negated the magnitude of the conversion **before** the
  test of the range: `-7` became `2^64-7`, so every negative number answered
  `LLONG_MIN` with `ERANGE`, `atoi("-7")` answered `0`, and a program that read
  a signed number of a file or of a line of a command read a wrong one. The
  sign is applied after the range, and the test holds the negative numbers of
  the conversion, both ends of the range and the end pointer.

## [1.6.0] - 2026-11-23

### The release

- 1.6.0-rc1: the release of the memory a program maps, the account of a
  task, the interrupts of many cores, the disk on the bus, the pipes at the
  speed of the memory and the machine in the network (`docs/releases/1.6.md`).
- 1.6.0-rc2: the 600-second session of the tag refused to build its image -
  the programs of the release no longer fit the 8 MiB of that volume
  (`mkfs: /bin/sqlite3 took -4 byte(s) of 848800`). The volume of the session
  is 24 MiB and the filler of free space steps down by 256 KiB, because a
  single file near the free count of the volume is refused by the chain and
  the table and not by the count.


### Added

- A server of the files of the volume, `httpd serve [port] [root]`
  (ADR 0036): HTTP/1.1 with keep-alive, GET and HEAD, `Content-Length`, the
  type from the extension, `ETag` and `304`, one `Range` with `206` and
  `416`, a listing of a directory as a chunked page of links, a `301` for a
  directory named without its slash, a line of `/var/log/httpd.log` per
  request, an idle timeout, a ceiling of the sessions that answers `503`, and
  a path that is decoded and walked under the root, where a `..` that would
  climb out is `403`.
- The serving side of TLS, `httpd tls [port] [root]` on 443: the vendored
  mbedTLS with `MBEDTLS_SSL_SRV_C`, the certificate and the key of
  `/etc/ssl/server.crt` and `server.key` through the VFS, and `curl -k`
  against the x86-64 guest in `make x86-net-smoke`. The board has no TLS on
  either side and says so.
- An update over the network, `ota fetch <url>` (ADR 0022 §7, ADR 0036): the
  signed image is downloaded into a staging file, the trailer and the
  Ed25519 signature are checked **before** the inactive slot is written, the
  slot is selected and the reboot is the caller's; a slot that never
  confirms itself is rolled back - in the file form of x86-64 and in otadata
  on the board. `ota status`, `ota boot` and `ota confirm` are the state at
  the console, and `wget --sha256` reports the digest of what it read.
- The system call of the x86-64 port takes the fast door of the processor
  (ADR 0035): the library of a program calls with `syscall` and returns with
  `sysret`, the port programs `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR` and
  `IA32_FMASK` on every core, and the entry finds the kernel stack of the
  program in the per-core record under `IA32_GS_BASE` - the same number
  `port_x86_64_tss_set_rsp0()` writes into `RSP0` of the TSS. The frame is the
  same 160 bytes in the same order as the vector 0x80 leaves, so the signal
  frame, the child of a `fork()` and the replacement frame of an `execve` did
  not move; `sysret` is guarded (a canonical address and a ring 3 selector, or
  the frame goes back through `iretq`), because the frame of a `sigreturn`
  comes from memory the program can write. The vector 0x80 stays as the second
  door: it is what a program of another compiler calls and what the restorer
  of a signal handler returns through, and it now leaves through the same fast
  tail. A `getpid()` of a program costs 0.55 µs instead of 0.92 µs on the QEMU
  of the tests - 1.8 million calls a second instead of 1.1 - and a `grep`,
  `sed` or `wc` of a file of 1 MiB moves by less than the noise, because the
  door is not what those commands spend their time in.
- The grain of the I/O (ADR 0035): the ring of a pipe is a setting of the
  port - 64 KiB on x86-64 and the host, the block of one call of the system,
  4 KiB on the board - and comes from the allocator with the pipe, so sixteen
  pipes of 64 KiB cost a megabyte only when they are all open and the board
  pays 4 KiB per stage; a write of `PIPE_BUF` bytes waits for the whole block
  and lands whole, and the wait of a pipe has no bound - a reader slower than
  its writer sleeps instead of answering `EAGAIN`, and a signal cuts the wait
  short with `EINTR`. A stream has a real block from the arena of the program
  and `setvbuf` works: `_IOLBF` on a terminal, `_IOFBF` on a file and a pipe
  (4 KiB), `_IONBF` for the error descriptor, `fflush`, `fclose` and `exit`
  over it, and `printf` reaches the block of `stdout` through a weak seam
  when the program links the streams. `/bin/streams` checks the three modes
  and the flush of `exit`; `/bin/iobench` and `tools/io_bench.py` are the
  ruler of the machine. One megabyte through `cat file | wc -c` went from
  5.9 s to 0.47 s, the same megabyte through `iobench pipe` from 0.20 to
  29 MiB/s with the reader's calls down from 4096 to 16, and a filter of
  sbase over a megabyte of text is 1.2-1.5 times faster (its time is the
  regular expression, not the calls).
- The accounts of the machine are files of `/etc` (ADR 0033): `/etc/passwd`
  holds `name:uid:gid:home:shell` and `/etc/shadow` the salted SHA-256 of a
  password (`$sha256$<salt>$<hex>`, `*` for an account without one, mode
  `0600`), `getpwnam` and `getpwuid` read the same file as the shell, and
  `login`, `su` and `id` are commands of the console: `login` asks the name
  and the password three times and hands the shell to that user, `su` lets
  root become anybody without a password and a user become another only with
  the password of the account, `-l` enters the home, and `id` says who the
  shell is. `task_set_creds` moves the gid and the uid of a task in one
  critical section. There is no setuid bit in `MOX1` and no gid in the record
  of a filesystem: the console is the door, and both are a format change with
  an ADR of their own. `make x86-smoke` boots the machine, logs in as root
  with a wrong and then the right password, becomes uid 7 with `su -`, refuses
  `su root` with the wrong password, and reads the owner of a file the user
  wrote; 340 checks.
- The disk of the x86-64 port speaks bus-master DMA (ADR 0032): the PCI IDE
  function is found by its class, the bus-master bit is set in its command
  register, a PRDT of 4 KiB pieces (no entry crosses a page) moves up to
  64 KiB in one command, and the transfer ends with IRQ 14 instead of a poll
  of the status register. A machine without the function, a drive whose
  identify data says it cannot and a buffer whose pages the port cannot
  translate all keep the PIO path, which is unchanged.
- ADR 0032 states the contract of a physical address: which addresses may
  reach a device, who translates them (`port_x86_64_bus_phys` for the heap
  and the image, `port_x86_64_paging_phys` for a page of a program), what
  keeps the memory alive while the transfer runs, and why x86-64 needs no
  cache maintenance around a DMA.
- `/proc/blockdev` lists the block devices with the state of the cache above
  each of them: the bytes of a sector, the sectors of the device, the boxes,
  the boxes that hold a sector, the dirty sectors the medium does not have
  yet, and whether the cache writes back.
- `k_blockdev_register_cached_sized()` and `k_blockdev_cache_stats()`: a
  device asks for a cache of its own size (the disk of the x86-64 port asks
  for 512 sectors, half a megabyte, because a bus master moves a run at a
  time) and a reader can ask what it holds.
- A task carries a uid and a gid, and the triads of a file are read against
  them: `getuid`, `geteuid`, `getgid`, `setuid` and `setgid` take the numbers
  36 to 40, root passes every triad while anyone else gets the bits of the
  owner or of "others" (no filesystem of this system stores a group), `chmod`
  is the owner's or root's, `chown`, `mount` and `umount` are root's, and
  `kill` of a task of another uid answers `EPERM`. The credentials are
  inherited by `task_create` and by `fork`, and `execve` keeps them.
- `rename` reaches ext2, and `rmdir` is a call of its own (41) in the VFS and
  in MicroFS, FAT32 and ext2: `unlink` of a directory is `KERR_ISDIR`, `rmdir`
  of a file is `KERR_NOTDIR`, a full directory is `KERR_NOTEMPTY`, and
  `rm -r` and `/bin/rmdir` use the call.
- `fstat` (42) answers the mode, the owner, the size and the two moments of
  the object behind an open descriptor, where the library used to answer 0666
  and the size through `lseek`; `O_EXCL` is a flag of `open` and of the ABI.
- `ext2_repair` and `fsck ext2 [-y] <point>`: the counters of the superblock
  and of every group, both bitmaps, the link counts, the `.` and `..` entries
  and the size of a directory against its chain, and a lost file reconnected
  as `#<inode>` under `/lost+found`. The oracle is the host: `e2fsck -fn`
  answered "Free blocks count wrong" with exit 4 before the repair and exit 0
  after it.
- The map of `mfs_check` is sized by the volume instead of a constant of 32 768
  records, so a volume of 40 000 records repairs an orphan past the old
  ceiling.
- SQLite keeps its journal in a file (`journal_mode=DELETE`): `make
  x86-fs-smoke` cuts a transaction with a reset and reads the rolled-back rows
  on the next boot, `PRAGMA integrity_check` answering `ok`.
- The switch of a task saves and loads the XMM and x87 state (512 bytes of
  `struct port_task`, `fxsave64` and `fxrstor64` in `commit_switch()`), a
  `fork` copies the live state into the child, `execve` starts the new image
  from a clean template, and the trampoline of an application processor turns
  `CR4.OSFXSR`/`OSXMMEXCPT` and `CR0.NE` on - without them SSE in ring 3 of
  the second core was `#UD`. `/bin/fpu2` is the test that failed before it:
  two children compute in `xmm0`/`xmm1` while the parent computes, and a third
  dirties x87 and the control words before an `execve`.

- A program has the POSIX process pair on x86-64: the child of `fork()` is a
  task with a copy-on-write image, `execve()` from a program replaces the
  image of the caller instead of starting a second task (the pid, the parent,
  the descriptors and the directory stay), and `waitpid()` reaps the child and
  answers its status, with `-1` meaning "any child". `/bin/procs` proves the
  four shapes in `make x86-smoke`: a child that computes 55 and returns it, a
  double fork with two statuses, and a child whose `execvp` becomes another
  program. `make x86-smoke` is 319 checks. The shell stays the synchronous
  caller it was, because a kernel task has no address space to replace.
- A pipeline of the shell is no longer three stages: the parser holds sixteen
  (the most a line of 32 words can carry), the pipes are made one boundary at
  a time as the stages are forked - the shell keeps one reading end between
  two forks and closes the boundary behind it - and the system has sixteen
  pipes and forty open-file descriptions. A line of ten stages and Ctrl+C for
  the whole job are in `make x86-smoke`.
- `xargs -P` works: the command of sbase forks a child per batch, the child's
  `execve` replaces it, and the parent reaps with `waitpid(-1)` without
  blocking below the ceiling of `-P`. `make x86-smoke` runs six commands with
  `-P 4` and checks that each word was handed to one child.
- The interface has programs (ADR 0030): `/bin/edit` is a text editor on the
  screen - a file, the arrows, the characters of the keyboard, `Ctrl+S` to
  save and `Ctrl+Q` to leave, a title bar, a status line and a modal window
  that asks about an unsaved file - and `/bin/fm` is a file manager: the
  directory through `readdir`, the selection, Enter to open a directory or
  hand a file to `/bin/edit`, `d`/Delete to `unlink` after a modal question
  and `q` to return. Both write only the rows they changed. `make
  x86-vga-smoke` boots a copy of the disk, types two lines into the editor
  with `sendkey`, saves, reboots and reads the file back with `cat`, and
  lists, moves and deletes in the file manager; 129 checks.
- The window model, the canvas and the keyboard are `components/ui`, portable
  and tested on the host (`tests/cases/test_ui_windows.c`): a window is a
  rectangle in the order of the paint with a focus and a modal flag, the
  canvas fills, outlines and lays out a line of cells of a font the caller
  gives, and the keyboard maps the names of `/dev/input0` to the characters
  of the US layout. `/bin/ui` uses the model for its two windows.
- The arrows, Home, End, PageUp, PageDown, Insert and Delete are keys of the
  machine: the E0 prefix of the scancode set 1 is translated by the portable
  table `kernel/input_set1.h` instead of being dropped, the ten numbers are
  appended to `enum k_input_key` (69-78) and never move, `key.up` and
  `key.delete` are events of `/dev/input0`, the right Ctrl is a modifier of
  the same form, and the Enter and the slash of the keypad still type their
  character for the shell. `tests/cases/test_ui.c` holds the numbers and the
  table.
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
- The timer of the PIT and the keyboard of the x86-64 machine are lines of the
  I/O APIC and no longer of the 8259: the port reads the table of the firmware
  (the address of the chip and the ISA overrides of the MADT), programs one
  redirection entry per line - the vector, the destination core, the polarity,
  the trigger and the mask - masks the legacy chip as a whole and sends the EOI
  to the local APIC of the core that took the line. A machine whose firmware
  describes no chip keeps the 8259, and `smp irq` prints the table;
  `smp irq keyboard <cpu>` moves a line to another core, which is how
  `make x86-smp-smoke` sees the keyboard arrive where it was sent (ADR 0034).
- A task that the scheduler puts in the queue of another core wakes that core
  with an inter-processor interrupt instead of waiting for its next tick: the
  port contract gains `k_port_cpu_wake()` (weak in `src/cpu_default.c`, so a
  port with one core keeps the tick), the scheduler calls it after it drops
  its lock, and the x86-64 port sends the vector `0x51`, whose body claims and
  switches. The port counts the tasks that began before the tick of the core
  and the ones that began at it, and `smp wake` stops the clock of the other
  core to show that an interrupt - and nothing else - starts a task there
  (ADR 0034). `make x86-smp-smoke` is 52 checks.
- The image of `make x86-disk` carries the two files an `https://` repository
  needs on a fresh machine: the bundle of roots of the Mozilla program in
  `/etc/ssl/certs.pem` (`tools/ca/README.md`) and the key of the repository of
  the project in `/etc/pkg/keys/microos-progs.pub` (`tools/pkg/README.md`).
  The documented `pkg repo add` + `pkg -Sy` + `pkg -S hello` installs a
  package out of the box, and `make x86-net-smoke` checks it on the image of
  the release: 59 checks.

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

- `waitpid(-1)` and `wait()` could not wait for any child: the pid arrives in
  an unsigned register, where -1 is larger than every pid, and the scan of the
  children compared it as a number - the wait answered `ECHILD` and `xargs`
  could not reap the children it had forked. The scan compares the signed
  value, and a host test waits for `-1`.
- The parent's first write after a `fork()` went into the frame the child was
  about to return into: the clone made both page tables read-only but flushed
  the TLB with an address 32 MiB above the page (the base of the window was
  counted twice), so the stale writable entry stayed. The clone flushes the
  address it changed, and a libc program's child returns 0 without the
  library's `getpid()` workaround and its yield.

- A pipeline lost the output of its last stages: the shell keeps no
  descriptor of its own, so `pipe()` handed out the low numbers and a read
  end of a boundary could land on descriptor 1 - the standard output of the
  stage, which then answered `bad file descriptor` (`15 stages` of
  `cat | ... | wc -l` printed no count). `stage_setup()` moves the two ends
  above 2 and closes the three before the copies, so a stage owns exactly 0
  and 1 whatever the numbers of the pipes were.
- The documented maximum of a chain did not run: sixteen stages of programs
  need 34 tasks of a table of 32 (a stage is two tasks, the stage and the
  program) and the last ones were never started, silently. The shell counts
  the task slots before it makes the first pipe and refuses a chain that does
  not fit with the numbers; fifteen stages of programs fit on x86-64 and
  sixteen of the commands of the shell, and `docs/wiki/shell.md` says why.
- `pkg` could not download anything over `https` on an image out of the box:
  `/etc/ssl/certs.pem` was not there, a certificate signed with
  ecdsa-with-SHA384 was `signature algorithm (oid) is unsupported` because
  SHA-384 is a module of its own in mbedTLS 3.6 (ISRG Root X2 and the P-384
  chain of Let's Encrypt were unreadable), and one certificate the image does
  not implement made the library refuse the whole bundle. SHA-384 is enabled,
  a bundle is loaded whole with the certificates that cannot be parsed skipped
  and counted once in the log, the head of an answer is read up to 1024 bytes
  (GitHub Pages answers 702, and the old 640 said "the address or the answer
  is too long"), and `pkg` names the key file when a signature does not match.
- `kill -TERM <pid of the shell>` left the machine without a console until a
  reset: the console is the only shell and the port has no `getty`
  (ADR 0033). The signal to the task that runs the command is refused with a
  sentence, as the one to `init` already was.
- A name longer than a volume keeps was `file too big` or `invalid argument`:
  `mkdir`, `write`, `rm`, `stat`, `ls`, `tree` and `df` of the shell say
  `file name too long`, the limit of MicroFS (32 bytes) named as the limit it
  is.
- The `mkdir` of the shell took one directory and dropped the other words of
  the line in silence; every word is a directory now, as in Linux and in the
  program of `/bin`.
- `df` printed `(0 KB each)` for the 512-byte blocks of a FAT32 volume and
  `inodes: 0 of 0 free` for a volume that keeps no inodes. The unit of the
  block is named (`512 bytes each`, or `1 KB each`), and a volume without
  inodes says `not kept by this volume`.
- `login` with no name said `the name does not fit` and then `no such
  account`; it says `a name is needed`, and nothing else.
- The help of `cat` promised `-n` without saying that on x86-64 the `/bin/cat`
  of sbase runs at the prompt and takes `-u`.

## [1.5.0] - 2026-11-23

### Added

- A task carries a uid and a gid, and the triads of a file are read against
  them: `getuid`, `geteuid`, `getgid`, `setuid` and `setgid` take the numbers
  36 to 40, root passes every triad while anyone else gets the bits of the
  owner or of "others" (no filesystem of this system stores a group), `chmod`
  is the owner's or root's, `chown`, `mount` and `umount` are root's, and
  `kill` of a task of another uid answers `EPERM`. The credentials are
  inherited by `task_create` and by `fork`, and `execve` keeps them.
- `rename` reaches ext2, and `rmdir` is a call of its own (41) in the VFS and
  in MicroFS, FAT32 and ext2: `unlink` of a directory is `KERR_ISDIR`, `rmdir`
  of a file is `KERR_NOTDIR`, a full directory is `KERR_NOTEMPTY`, and
  `rm -r` and `/bin/rmdir` use the call.
- `fstat` (42) answers the mode, the owner, the size and the two moments of
  the object behind an open descriptor, where the library used to answer 0666
  and the size through `lseek`; `O_EXCL` is a flag of `open` and of the ABI.
- `ext2_repair` and `fsck ext2 [-y] <point>`: the counters of the superblock
  and of every group, both bitmaps, the link counts, the `.` and `..` entries
  and the size of a directory against its chain, and a lost file reconnected
  as `#<inode>` under `/lost+found`. The oracle is the host: `e2fsck -fn`
  answered "Free blocks count wrong" with exit 4 before the repair and exit 0
  after it.
- The map of `mfs_check` is sized by the volume instead of a constant of 32 768
  records, so a volume of 40 000 records repairs an orphan past the old
  ceiling.
- SQLite keeps its journal in a file (`journal_mode=DELETE`): `make
  x86-fs-smoke` cuts a transaction with a reset and reads the rolled-back rows
  on the next boot, `PRAGMA integrity_check` answering `ok`.
- The switch of a task saves and loads the XMM and x87 state (512 bytes of
  `struct port_task`, `fxsave64` and `fxrstor64` in `commit_switch()`), a
  `fork` copies the live state into the child, `execve` starts the new image
  from a clean template, and the trampoline of an application processor turns
  `CR4.OSFXSR`/`OSXMMEXCPT` and `CR0.NE` on - without them SSE in ring 3 of
  the second core was `#UD`. `/bin/fpu2` is the test that failed before it:
  two children compute in `xmm0`/`xmm1` while the parent computes, and a third
  dirties x87 and the control words before an `execve`.

- A program has the POSIX process pair on x86-64: the child of `fork()` is a
  task with a copy-on-write image, `execve()` from a program replaces the
  image of the caller instead of starting a second task (the pid, the parent,
  the descriptors and the directory stay), and `waitpid()` reaps the child and
  answers its status, with `-1` meaning "any child". `/bin/procs` proves the
  four shapes in `make x86-smoke`: a child that computes 55 and returns it, a
  double fork with two statuses, and a child whose `execvp` becomes another
  program. `make x86-smoke` is 319 checks. The shell stays the synchronous
  caller it was, because a kernel task has no address space to replace.
- A pipeline of the shell is no longer three stages: the parser holds sixteen
  (the most a line of 32 words can carry), the pipes are made one boundary at
  a time as the stages are forked - the shell keeps one reading end between
  two forks and closes the boundary behind it - and the system has sixteen
  pipes and forty open-file descriptions. A line of ten stages and Ctrl+C for
  the whole job are in `make x86-smoke`.
- `xargs -P` works: the command of sbase forks a child per batch, the child's
  `execve` replaces it, and the parent reaps with `waitpid(-1)` without
  blocking below the ceiling of `-P`. `make x86-smoke` runs six commands with
  `-P 4` and checks that each word was handed to one child.
- The interface has programs (ADR 0030): `/bin/edit` is a text editor on the
  screen - a file, the arrows, the characters of the keyboard, `Ctrl+S` to
  save and `Ctrl+Q` to leave, a title bar, a status line and a modal window
  that asks about an unsaved file - and `/bin/fm` is a file manager: the
  directory through `readdir`, the selection, Enter to open a directory or
  hand a file to `/bin/edit`, `d`/Delete to `unlink` after a modal question
  and `q` to return. Both write only the rows they changed. `make
  x86-vga-smoke` boots a copy of the disk, types two lines into the editor
  with `sendkey`, saves, reboots and reads the file back with `cat`, and
  lists, moves and deletes in the file manager; 129 checks.
- The window model, the canvas and the keyboard are `components/ui`, portable
  and tested on the host (`tests/cases/test_ui_windows.c`): a window is a
  rectangle in the order of the paint with a focus and a modal flag, the
  canvas fills, outlines and lays out a line of cells of a font the caller
  gives, and the keyboard maps the names of `/dev/input0` to the characters
  of the US layout. `/bin/ui` uses the model for its two windows.
- The arrows, Home, End, PageUp, PageDown, Insert and Delete are keys of the
  machine: the E0 prefix of the scancode set 1 is translated by the portable
  table `kernel/input_set1.h` instead of being dropped, the ten numbers are
  appended to `enum k_input_key` (69-78) and never move, `key.up` and
  `key.delete` are events of `/dev/input0`, the right Ctrl is a modifier of
  the same form, and the Enter and the slash of the keypad still type their
  character for the shell. `tests/cases/test_ui.c` holds the numbers and the
  table.
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

- `waitpid(-1)` and `wait()` could not wait for any child: the pid arrives in
  an unsigned register, where -1 is larger than every pid, and the scan of the
  children compared it as a number - the wait answered `ECHILD` and `xargs`
  could not reap the children it had forked. The scan compares the signed
  value, and a host test waits for `-1`.
- The parent's first write after a `fork()` went into the frame the child was
  about to return into: the clone made both page tables read-only but flushed
  the TLB with an address 32 MiB above the page (the base of the window was
  counted twice), so the stale writable entry stayed. The clone flushes the
  address it changed, and a libc program's child returns 0 without the
  library's `getpid()` workaround and its yield.

## [1.4.0] - 2026-11-23

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

## [1.3.1] - 2026-11-23

A patch of `1.3.0` for the three defects the first copyleft packages and the
600-second session of the tag found. The volume, the syscall ABI (the calls
of 1.3.0 and no other) and `MOX1` do not change.

### Fixed

- `execve` waited for a program with a budget of ten seconds and **returned**
  when it was spent, leaving the task running with the console of the shell
  and a slot nobody reaps: the session of the tag caught a table of four tasks
  where two ran. The budget is a minute, and a program that outlives it is
  stopped with `SIGTERM` and reaped.
- the skip of a field the installer does not know read the opening quote of a
  string as its closing one, so one `homepage` of an index made the whole
  document unreadable - against the promise of `docs/wiki/pkg.md`.
- `rename` handed its two names to the kernel untouched while `open`, `stat`
  and `unlink` resolve them in the library: a relative name answered `EINVAL`
  for `rename` alone, and a program that saves through a temporary name lost
  the file.
- the shell printed `the program has finished` into the stream of the
  program, so with a redirection the sentence landed in the file: the port of
  `gzip` caught it as a trailing garbage in a `.gz` it then refused. The line
  goes to the error stream.

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
