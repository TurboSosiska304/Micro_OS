# SQLite — the database library of the public domain

SQLite is a relational database in one C source, in the public domain. This
tree carries the amalgamation of the release and builds it as a static
archive of the x86-64 port, linked into `/bin/sqlite3`; the library is also
compiled for the host, and `tests/cases/test_sqlite.c` is its first machine.

* Upstream: <https://www.sqlite.org/>
* Release: 3.54.1, 2026-10-16
* Source id: `555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76`
* Amalgamation: <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>
* License: [`LICENSE`](LICENSE), the public-domain statement of
  <https://www.sqlite.org/copyright.html> (the deliverable code is public
  domain; the build scripts of the release are not part of the amalgamation)
* Used in: `/bin/sqlite3` of the x86-64 MicroFS image, and the library of the
  host test
* Built by: `cmake/userland.cmake` with the compiler and the flags of the
  guest (`-nostdlib`, `-ffreestanding`, no PIC) against
  `components/libc/user`; the program is built **without**
  `-mgeneral-regs-only` because a `REAL` is a `double` and the port turns the
  SSE and x87 units on for it
* Recipe: [`docs/porting-programs.md`](../../docs/porting-programs.md)

## What is here

```
sqlite3.c               the amalgamation, unchanged
sqlite3.h               the API, unchanged
shell.c                 the shell of the release, compiled through port/sqlite_shell.c
LICENSE                 the public-domain statement
port/sqlite_vfs.c       the VFS: the operating system of SQLite on this system
port/sqlite_port.h      the two numbers of the port (the path and the VFS name)
port/sqlite_shell.c     shell.c with its entry renamed (no hunk of the source)
```

The upstream files are **not edited**. The shell is compiled whole by
`port/sqlite_shell.c`, which defines `main` as `sqlite3_shell_main` before it
includes the file; the program of the image (`programs/sqlite3.c`) has the
`main` that prepares the console and the input of this machine.

## The port layer

`SQLITE_OS_OTHER=1` leaves SQLite without its `unix` (or Windows) layer, and
`port/sqlite_vfs.c` is the whole operating system it sees. It is one file with
two builds: the calls of `components/libc/user` on the guest, the libc of the
host in the unit test (`SQLITE_PORT_HOST`).

| Method | What it does |
|---|---|
| `xOpen`, `xClose` | `open` with the flags of POSIX, the mode 0644 of a new file, `SQLITE_OPEN_DELETEONCLOSE` remembered; a master journal is refused |
| `xRead`, `xWrite` | `lseek` to the offset and a loop of `read`/`write`; a short read fills the rest with zero and answers `SQLITE_IOERR_SHORT_READ` |
| `xTruncate` | the host asks `ftruncate`; the guest has no such call, so the first bytes are copied to `<name>.trunc` and the copy is renamed over the file (a debt, see below) |
| `xSync` | the host asks `fsync`; on the guest it is a no-op because MicroFS writes the block in the call and the ABI has no flush |
| `xFileSize` | `lseek` to the end |
| `xLock`, `xUnlock`, `xCheckReservedLock` | no-ops that remember the level: one machine, one task, no second process; `SQLITE_FCNTL_LOCKSTATE` answers the truth |
| `xFileControl` | `SIZE_HINT`, `CHUNK_SIZE` and `SYNC_OMITTED` are accepted, `LOCKSTATE` is answered, everything else is `SQLITE_NOTFOUND` |
| `xSectorSize` | 512, the sector of the medium (ADR 0022) |
| `xDeviceCharacteristics` | 0: no sector is atomic and no write happens in an order the filesystem promises |
| `xDelete`, `xAccess` | `unlink` and `stat`; a missing file is not an error of a delete, and `READWRITE` also asks `access(W_OK)` |
| `xFullPathname` | an absolute name is copied; a relative one is joined to `getcwd` (the virtual cwd of the program) |
| `xRandomness` | a linear generator seeded from `uptime`, the pid and the address of the VFS; **not** cryptographic (a debt) |
| `xSleep` | `usleep`, the millisecond sleep of the ABI rounded up |
| `xCurrentTime`, `xCurrentTimeInt64` | the **uptime**: the machines have no wall clock, so the Julian day is 1970-01-01 plus the seconds since the boot, in the two shapes SQLite asks (a debt) |
| `xGetLastError` | `strerror(errno)` |
| `sqlite3_os_init`, `sqlite3_os_end` | register the VFS as the default, and do nothing on the way out |

The journal is the rollback journal of SQLite, never the write-ahead log
(`SQLITE_OMIT_WAL`): WAL needs shared memory and a mapping, and the machine
has neither. The VFS implements a journal file; the **program** of the guest
asks for `journal_mode=MEMORY`, because a task of this machine has four
descriptors and one is the database, two are the console, and a `.read` of a
script needs the fourth (see the debt below). The host test runs DELETE and
PERSIST over the same VFS.

## The flags

```
SQLITE_OS_OTHER=1                the VFS above and no unix layer
SQLITE_THREADSAFE=0              one task, no mutex of the library
SQLITE_OMIT_LOAD_EXTENSION=1     no dlopen
SQLITE_OMIT_WAL=1                no shared memory, no mmap
SQLITE_OMIT_DEPRECATED=1
SQLITE_DQS=0                     no string literal where an identifier belongs
SQLITE_TEMP_STORE=3              the temporary store in memory
SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576
SQLITE_DEFAULT_CACHE_SIZE=-256   256 KiB of page cache in the arena
SQLITE_DEFAULT_PAGE_SIZE=4096
SQLITE_NOHAVE_SYSTEM=1           no `.system` and no `.edit`: no shell to start
```

`-Os` keeps the image at about 823 KiB (`text 675808`, `data 166816`); the
arena is `ULIBC_HEAP_SIZE=786432` and the bss of the image is 789640 bytes,
under the megabyte the format allows. The image is under the 8 MiB ceiling of
`K_EXEC_MAX_IMAGE` as well as under the 16 MiB window of the port.

The shell has no readline (the header is not in the library of a program), so
a line is read with `fgets`; `-batch`, `-cmd`, `.read`, `.tables`, `.schema`
and `.mode` are the shell of the release.

## The program

`programs/sqlite3.c` is the console and the input of this machine:

1. the script of stdin is read whole into the arena (the pipe may answer
   `EAGAIN` while its producer writes), the descriptor 0 is closed, and
   `stdin` becomes a stream over the memory (`fmemopen`). The shell then reads
   the script from the arena, and a `.read` of the shell has descriptor 0 for
   as long as it runs;
2. `/dev/tty0` is opened and `dup2`'d onto 1 and 2. The table is inherited
   from the stage of the shell, so the numbers 1 and 2 are not free just
   because the stage asked for no redirect; without this a database opened on
   descriptor 1 would receive the output of the program;
3. the slot 3 is closed, so the database takes 0 and the next file SQLite
   opens takes 3;
4. the arguments get `-cmd "PRAGMA journal_mode=MEMORY"` in front, the one
   command of the port.

The four descriptors of the program, then: 0 the database (or a `.read` while
one is open), 1 and 2 the console, 3 free.

## Known limits

* **No wall clock.** `time()` is `uptime`; a `CURRENT_TIMESTAMP` written by
  the guest is 1970-01-01 plus the seconds of the boot. `docs/debts.md`.
* **The journal of the program is in memory.** A write transaction of
  `/bin/sqlite3` is rolled back correctly after an SQL error, but a power cut
  in the middle of a transaction cannot be repaired by a journal on the
  medium. DELETE and PERSIST are implemented by the VFS and exercised by the
  host test; the guest takes MEMORY because of the four descriptors. A wider
  table of descriptors (K4) lifts it.
* **`xTruncate` copies the file.** The ABI has no truncate. A rollback that
  shrinks the database copies the first pages to a temporary name and renames
  it; the data is right, the cost is one more pass, and a power cut between
  the rename and the reopen leaves `<name>.trunc` to the check of the volume.
* **Randomness is a generator of the port**, not an entropy device; it seeds
  `randomblob()` and the name of a temporary file.
* **One machine, no locks.** Two processes of this system do not share a
  database; the VFS answers the lock calls but no second task is kept out.
* **`.system`, `.edit` and `.output |cmd` are absent**: the library has no
  `system` (the option) and no `popen` (`ENOSYS`).
* **Temp files and ATTACH.** `SQLITE_TEMP_STORE=3` keeps the temporary store
  in memory. An `ATTACH` of a second database works only while a descriptor
  is free, and the descriptors of a program are four.

## The tests

* `tests/cases/test_sqlite.c` — the host: the VFS as the default, a created
  file, a table with INTEGER/TEXT/REAL/BLOB, an update, a delete, a
  transaction that rolls back, DELETE and PERSIST journals, `PRAGMA
  integrity_check` and a reopen of the same file.
* `make x86-smoke` — `/bin/sqlite3` of the image: the table of `/etc/sql-*.sql`
  is created, read in a second process, read through `.read`, shown through
  `.tables`/`.schema`/`-cmd`, and its integrity check says `ok`.
* `make x86-disk-smoke` — the database written in the first boot of the disk
  is read and checked in the second one, after the reset.

The license: SQLite is in the public domain. The image carries
`/etc/licenses/sqlite`, `tools/bundle.sh` puts `LICENSE.sqlite` into the
x86-64 archive, and `docs/licensing.md` has the row.
