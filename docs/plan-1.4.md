# The road to 1.4.0-rc1

The release a person can hand to somebody else: a machine whose root may be
any filesystem of the tree, whose big volumes are ordinary, whose programs
come from a repository that carries copyleft honestly, and whose screen has
the first shape of an interface. This page is the plan; the decisions live in
ADRs, the state in `docs/roadmap.md`.

## What 1.4.0-rc1 must show

1. **Every filesystem of ADR 0009 as a root and as a mount point**:
   MicroFS, FAT32 and ext2 - mounted at `/`, and any of them mounted under
   any other on a second device or a second partition.
2. **Five copyleft programs** in `microos-progs`, installable with `pkg`
   from the signed repository, each with its license and its exact source.
3. **Large volumes**: a 5 GiB volume is created, mounted, written, read and
   checked without drama - and without a bitmap in the kernel.
4. **A skeleton of a UI**: the framebuffer shows more than a console, the
   keyboard reaches it, and the machine is still a shell when the UI is not
   running.

## M0 - 1.2.1, a patch of 1.2.0 (first, because of a bug)

`fread` after `ungetc` writes one byte past the caller's buffer and shifts
the rest; `fseek(SEEK_CUR)` counts from the descriptor instead of the stream
(the 512-byte block of 1.2.0 sits between them). The first is memory
corruption and 1.2.0 shipped it.

* the two bugs, each with a host test that fails before the fix;
* `mkprog.sh`: the data segment is `bss address - data address`, not the sum
  of the section sizes;
* quotes in the shell: `'...'`, `"..."`, `\` - `awk '{print $1}'` and
  `grep 'a b'` work;
* gate: host/ASan/TSan, `x86-smoke`, the disk, the network, the VGA, the ISO,
  the simulator, the board (3 minutes and the device test), `soak` 600 s;
* tag `v1.2.1`.

## M1 - 1.3.0: many cores and the ports

* `feature/smp2`: 2 reported and 2 in use, 34/34, `soak-smp` 600 s, the
  I/O APIC still a stub (named in the debts);
* the ports cut from 1.2: zstd, file (`-z` through zstd), sqlite, awk - one
  integration pass, the libc reconciled once (`stat`/`dirent`/`string_ext`
  shared, float from Lua, the formatter a superset);
* the small kernel work the ports ask for: `TASK_MAX_FDS` 16,
  `KSYS_FTRUNCATE`, `IMAGE_MB` 16, `pkg` asks for free space before a
  download;
* gate, tag `v1.3.0`.

## M2 - volumes and filesystems (1.3.x)

### M2.1 MicroFS without the ceiling

The bitmap of the free blocks is a static array of two blocks, so the volume
stops at 16 MiB for a reason that has nothing to do with the format. The
mount allocates the bitmap for the volume it finds (and `mfs_check` the
same), and the ceiling becomes the memory of the machine: 5 GiB is 640 KiB
of bitmap, 16 MiB is 2 KiB. On the board nothing changes - its volume holds
one block.

The write path is still quadratic (`chain_count` per write, `chain_last` per
grow): the cursor of 1.2 fixed the read side, the write side takes the same
shape.

### M2.2 ext2 that can be written

ADR 0019 gave the reader; a root must be writable. Block and inode
allocation, the bitmaps of a group, the insert and the remove of a directory
entry, `truncate`, `unlink`, `chmod`, `chown`, the times - and the two
checks that make it believable:

* every image we write passes `e2fsck -fn` on the host;
* the image mounts read-write on Linux and the bytes come back.

A minimal `fsck ext2` on the machine (superblock, bitmaps, the chain of the
directories) is part of it, because a machine that cannot check its own root
is a machine that trusts a crash.

### M2.3 A root that is not MicroFS

The port reads the partition type (and the filesystem, when the type is not
enough), `vfs` mounts the root with the type it found, and `init` reads
`/etc/init.d` from it. The release keeps MicroFS as the default image and
adds an ext2 image of the same contents; `x86-disk-smoke` boots both.

### M2.4 FAT32 to the end

`unlink`, `rename`, `truncate`, `mkdir` in the writer that already exists;
long names if they are cheap. FAT32 is the exchange format with the rest of
the world and the cheap second root.

### The gate of M2

* a 5 GiB ext2 volume: `mkfs`, mount as `/`, write 1 GiB of files, `df`
  honest, `fsck` clean, `e2fsck -fn` clean, a reboot finds the files;
* the same image mounted on the host: `mount -o loop`, the bytes agree;
* FAT32 as `/data` with a file written on the host and read on the machine;
* MicroFS as root with the others below it;
* no bitmap in the kernel for the 5 GiB volume (`df` and the heap prove it).

## M3 - the copyleft repository

* the index grows `license`, `source`, `source_sha256`; `pkg` refuses a
  copyleft package without a source (ADR 0025's `PKG_ELICENSE`);
* `make sdk`: a sysroot (headers, `libmicroos_user.a`, crt, linker script)
  and a CMake toolchain file, so a program is built outside this tree;
* five programs, GPL-2.0 or GPL-3.0, chosen for what they prove:
  `diff` (diffutils), `patch`, GNU `sed` or `grep`, `gzip`, `make`. Each is
  a package with `sources/`, `licenses/`, the patches of the port and a
  `BUILD` file; the CI of `microos-progs` builds and signs them;
* `pkg install diff` from the live repository over HTTPS, in a QEMU guest,
  is a smoke test - the whole promise of `pkg` in one command.

## M4 - the skeleton of a UI

A decision first (ADR): the UI is a **program** of the machine, not a second
kernel. It needs a framebuffer it may draw in and the keys it may read, which
is a device (`/dev/fb0`, `/dev/input0`) or an ioctl over the console - the
ADR names it.

The skeleton is: a panel (the clock, the tasks, the volumes), a list of
windows, one window holding the console, and the focus moving with a key.
No fonts beyond the Spleen of the console, no mouse, no widgets - the shape
that the next years fill in. The console stays the machine: when the UI is
not running, nothing of it is in the way.

## M5 - the gates and the release

* the numbers of every smoke, the three sessions (x86, the two cores, the
  board), `x86-user`, reproducible images, `integrity --expect-tag`;
* `docs/releases/1.4.md`, the wiki page, the landing, the notes;
* tag `v1.4.0-rc1`, then `v1.4.0`.

## What this plan adds to the four wishes

* **`termios`** (raw mode over an ioctl) is not in the four, and without it
  neither `less` nor `nano` nor an interactive UI input exists. It belongs
  to M4, before the widgets.
* **the wall clock** (RTC or NTP at the boot): ext2 timestamps, TLS, SQLite
  and the panel of the UI all ask for a date that is not the uptime.
* **the FPU state of a task** (`fxsave`/`fxrstor`): a GPL program with
  doubles and a shell that formats one are two float programs at once.
* **`e2fsck` and `mount -o loop` in CI**: the only oracle that says our
  ext2 is ext2. A filesystem we write and only we read is a story.
* **a package for the source**: `pkg` installs binaries; the source offer of
  GPL has to be a file of the repository that never moves, or the licence is
  not met. `sources/` with a digest in the index is the answer.
* **one image per root**: the release tar carries the MicroFS image and the
  ext2 image; `run.sh` chooses, and the smoke boots both.

## Order, and why

M0 first: 1.2.0 has a memory bug. M1 second: the ports are done and the
cores are done, and every later milestone builds on them. M2 and M3 are
parallel after M1 - one is the filesystem, the other the repository - and
they meet in M3's smoke, which installs a program onto an ext2 root. M4
last, because a UI on a filesystem that loses power is a toy.

## Decisions this plan takes, for the record

* ext2 is the big-volume and the second root; FAT32 is the exchange format;
  MicroFS stays the default of the release image and loses its ceiling.
* copyleft lives in `microos-progs` only, and a package without its source
  does not install.
* the UI is a program with a device, not a kernel subsystem.
* the ABI may grow (`ftruncate`, the framebuffer ioctl, the input device);
  no number is reused, no format of a file changes.
