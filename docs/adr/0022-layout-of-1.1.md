# ADR 0022 - The layout of 1.1: two slots, an update and the programs

* Status: accepted
* Date: 2026-11-22
* Supersedes: nothing
* Related: ADR 0004 (the port), ADR 0018 (programs of the board), ADR 0020
  (the signed image), ADR 0011 (the gate of 1.0)

## Context

ADR 0018 §6 says that the last **major** changes of the ESP32 are the work of
`1.1.0`: the settings of `/etc`, the programs in a partition of their own and
the update with an A/B slot and a rollback. From `1.2.0` the board receives
compatibility changes only, and a layout, a program format or a boot path
cannot change any more.

The layout of 1.0 is one application and the filesystem:

```
0x000000  boot loader
0x008000  partition table
0x009000  nvs                 20 KB
0x00E000  (unused)             8 KB
0x010000  factory              2 MB   (the trailer in its last 4 KiB)
0x210000  mfs               5.94 MiB
```

It has no second slot, so a board that is updated and then does not boot has
no image to return to, and the programs of `/bin` are blobs of the firmware
written into MicroFS at the boot (ADR 0018 §2), not files of the machine.

The flash is 8 MB and the volume of the user is the part that must not be
thrown away lightly. The question of this ADR is therefore not "may the board
grow" but "what is the final shape of the 8 MB, decided once", and the answer
must leave the user as much of it as the two slots and the programs allow.

## Decision

### 1. The table of 1.1, and the arithmetic of the 8 MB

```
# Name,     Type, SubType, Offset,    Size
nvs,        data, nvs,     0x9000,    0x5000
otadata,    data, ota,     0xE000,    0x2000
ota_0,      app,  ota_0,   0x10000,   0x120000
ota_1,      app,  ota_1,   0x130000,  0x120000
progs,      data, 0x41,    0x250000,  0x21000
mfs,        data, 0x40,    0x271000,  0x58F000
```

| Partition | Offset | Size | |
|---|---:|---:|---|
| reserved, boot loader, table | `0x000000` | `0x009000` | 36 KiB, the fixed part of the chip |
| `nvs` | `0x009000` | `0x005000` | 20 KiB |
| `otadata` | `0x00E000` | `0x002000` | 8 KiB, two sectors |
| `ota_0` | `0x010000` | `0x120000` | 1.125 MiB, of which the last 4 KiB is the trailer |
| `ota_1` | `0x130000` | `0x120000` | 1.125 MiB, of which the last 4 KiB is the trailer |
| `progs` | `0x250000` | `0x21000` | 132 KiB: two 64 KiB slots and the 4 KiB directory |
| `mfs` | `0x271000` | `0x58F000` | **5.56 MiB for the user** |
| | | `0x800000` | 8 MiB, nothing left over |

**The application slot is 0x120000 (1.125 MiB), not 2 MB.** The image of 1.1
is 993 664 bytes, so a slot leaves

```
0x120000 - 0x1000 (trailer) - 0x0F2980 (image) = 181 888 bytes (177.6 KiB, 15.4%)
```

for the growth of a compatibility port. That is the reserve ADR 0018 promises
to need: from `1.2.0` the board receives only the changes that are cheap on
this machine - a driver fix, memory, the console - and not a new format, a new
partition or a compiler. A slot of 1.1875 MiB (`0x130000`) would give 64 KiB
more of growth and take 64 KiB from the user; a slot of 1.25 MiB would give
128 KiB more for 128 KiB of the volume. The question of this ADR is the
opposite: the smallest slot that keeps the compatibility promise, so 1.125 MiB
is the answer. The build refuses an image that fits but leaves less than
64 KiB after its trailer (`tools/check_image_fits.cmake`, `MIN_FREE`): a 1.x
build that grows into the reserve is a decision about the layout, not a
silent squeeze.

The two slots have to be **equal** (the ESP-IDF bootloader and
`esp_ota_get_app_partition_count()` assume it) and every application
partition is **64 KiB aligned**; 0x10000 and 0x130000 are, and the sizes are
multiples of 0x10000. The **trailer of the signature (4 KiB, ADR 0020) is in
the last 4 KiB of each slot**: it belongs to the image it signs, so an update
of one slot leaves the pair of the other untouched. The application has
0x11F000 bytes for itself.

**`progs` is 0x21000 (132 KiB).** The programs are tiny (hello 2 152 bytes,
cat 1 920), but a program must start on a 64 KiB boundary of the flash for
`esp_partition_mmap`. The **directory is the last 4 KiB** of the partition
and the **two 64 KiB slots come first** (offsets 0 and 0x10000): hello and
cat fill them. There is **no reserve slot**: a program of 1.x is a new
`progs.bin` (a new image of the same partition, with the same two names or
another pair), not a slot that sits empty in the flash. A third program at
the same time would need a bigger partition, and that is a layout change the
port is not promised after 1.1; the 64 KiB it would cost is the volume of the
user, and the programs of the machine are two.

A directory in the first slot would make a 64 KiB partition hold no program
at all and a 192 KiB one hold only two: putting it at the end is what lets
two programs fit in 132 KiB.

**`otadata` is 8 KiB (two 4 KiB sectors)** between the NVS and the first
slot, the place the ESP-IDF bootloader reads; it holds two sequence entries
that alternate on every update. The **NVS is 0x5000 (20 KiB)**, the size the
board already had: the settings of `/etc`, the WiFi credentials and the
version floor of ADR 0020 live there, and 20 KiB is the ESP-IDF default for a
partition table of this size.

**MicroFS is everything that is left: `0x58F000` = 5 828 608 bytes =
5.56 MiB (5.83 MB decimal).** Against the 5.94 MiB (6 225 920 bytes) of 1.0
the user loses **0.38 MiB**: 2.25 MiB for the second slot (two slots of 1.125
instead of one of 2), 132 KiB for the programs, minus the 8 KiB of otadata
that 1.0 left unused. The documents of 1.0 said "5.75 MB"; the table of 1.0
is the 5.94 MiB above, and this ADR uses the bytes of the table from here on.

The **data of the user (MicroFS) survives an update** of an application slot:
an update writes only the inactive slot of 1.125 MiB and the 8 KiB of otadata.
A **flash of the new table** over a board of 1.0 is a one-time move: the
volume of 1.0 starts at 0x210000, which is now inside `ota_1`, and its bytes
are not a volume at 0x271000. The move therefore loses the volume once. The
release notes say it, `flash.sh --erase-volume` formats the new region in the
same pass, and a boot that finds a blank volume formats it by itself
(`mfs_vfs_mount()` already does that for a medium that was never written).

The new volume is 5.56 MiB; a boot of 1.1 writes `/log/start.log` (about
2 KB), `/etc` (about 1 KB) and the `/log/.foreign` test file (68 bytes), and
`progs` keeps `/bin` out of the volume entirely. The first `df` of a fresh
board reports more than 5.3 MiB free: the system does not need more than a few
kilobytes of MicroFS, and the rest is the user's.

### 2. The update and its rollback

The ESP-IDF bootloader with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` keeps
the state of a slot in otadata: `NEW`, `PENDING_VERIFY`, `VALID`, `INVALID`,
`ABORTED`.

1. `tools/ota_usb.py` reads otadata over USB, finds the slot that is **not**
   running, writes `micro_os.bin` and `micro_os.bin.sig` into it (the trailer
   at the last 4 KiB of that slot: `0x12F000` for `ota_0`, `0x24F000` for
   `ota_1`) and writes otadata: the other sector gets the next sequence and
   the state `NEW`. It does not touch the NVS, the bootloader, the partition
   table or MicroFS.
2. The board resets. The bootloader reads the new entry, changes `NEW` to
   `PENDING_VERIFY`, and starts the new image.
3. `app_main` checks the signature of the running slot before anything else
   (ADR 0020). A wrong trailer with the policy on is not kept: the slot is
   marked `INVALID` and `esp_ota_mark_app_invalid_rollback_and_reboot()`
   restarts the board into the other one. With no other image to run the
   board halts, which is the answer of the policy.
4. The shell task calls `esp_ota_mark_app_valid_cancel_rollback()` before its
   first line. At that point the kernel, MicroFS, `progs`, the self test, the
   hardware doctor and the console are up. The slot becomes `VALID` and a
   later reset stays in it.
5. A boot that ends before step 4 - a panic, the task watchdog, a halt, a
   power cut in the first seconds - leaves the entry in `PENDING_VERIFY`; on
   the next reset the bootloader marks it `ABORTED` and starts the other
   slot. Nothing of the application has to run to make the rollback happen.

No eFuse is burned and no secure boot is enabled: the rollback is a few
bytes of otadata, and a person who flashes the board can erase it. It is the
state machine of the ESP-IDF and not a second implementation.

`flash.sh` remains the **installation** path: it writes `ota_0`, its trailer
and `progs.bin`, and erases otadata, so a freshly flashed board starts in
`ota_0` whatever slot an older update selected. An update is
`tools/ota_usb.py` from the bundle (`python3 ota_usb.py /dev/ttyACM0`), which
also verifies the trailer with `microos_sign` before it writes anything when
the verifier is beside the bundle.

**The update over the network is not in 1.1.** The HTTP client of the kernel
is a text client with a bounded buffer, and `esp_ota_write()` wants a stream
of a megabyte with a rollback state around it, not a page read into a string.
A network update is therefore a debt with a next step (a streaming client
that writes into the inactive slot, the same confirmation and rollback, and a
policy for the URL and its signature). The USB path of this ADR is the safe
update the gate asks for, and it is the one `tools/device-test.sh` proves.

### 3. The signature of a slot

The envelope of ADR 0020 does not change: a 120-byte trailer (magic, version,
algorithm, size, SHA-256, Ed25519) after the bytes of the image, and the
public key in the firmware. What changes is **where the board looks**: at the
last 4 KiB of the partition it is running
(`esp_ota_get_running_partition()`), not at a fixed `factory` address, so the
same rule holds in `ota_0` and `ota_1`. `tools/bundle.sh` signs
`micro_os.bin`, and `flash.sh` / `ota_usb.py` put the trailer at
`0x12F000` / `0x24F000` respectively.

The check stays in `app_main`, which is electronic and not a boot-chain check
(ADR 0020 §6). What the second slot adds is that a damaged or foreign image
no longer needs a person with a cable: a refused signature marks the slot
invalid and the board returns to the image that worked.

### 4. `progs`: the programs are files of the partition

The table of the partition is in `components/port_esp32/include/
progs_format.h`:

```
the last 4 KiB  magic "MOPROGS1", version 1, count, slot, reserved
                count records of 36 bytes: name[24], offset, size, reserved
0x00000         the first program
0x10000         the second program
```

`tools/progs_image.py` writes the image at build time from the `.mox` files
that the component makes, checks the name, the size and the MOX1 header
(version 2, arch 1), and pads with 0xFF. The image is 0x21000 bytes, is
flashed by `idf.py flash` as the partition `progs`, and is part of the bundle
as `progs.bin`. There is no `target_add_binary_data` and no image of a
program in the firmware.

The port mounts the partition at **`/bin`** as a read-only filesystem
(`progs_vfs.c`): `ls /bin` lists the programs, `stat` reports mode 0755, and
`execve` reads the bytes with `esp_partition_read()` through the VFS. The
loader receives them exactly as it received the blob of 1.0, so the windows
of IRAM and DRAM, the door and the one-program-at-a-time rule do not change.

**Copy, not map.** A flash mapping would need a fixed virtual address
reserved by the first `esp_partition_mmap(..., ESP_PARTITION_MMAP_INST, ...)`
of the boot, a program linked for that address, and a guard against another
component mapping a page there first. The copy has none of that, it is the
path that 1.0 proved, and the price - an image in RAM for the time of the
copy - is the same 64 KB edge the kernel already allows (`KSYS_MAX_IO`). The
64 KiB alignment of the partition and of every file is kept for the mapping
that a later loader may take; it costs 64 KiB of the table and nothing else.

A program is replaced by flashing a new `progs.bin` (or a new image), not by
`cp` at the shell: the partition is read-only and belongs to the firmware,
which is also what makes `/bin` the list of what the machine can run. A
`.mox` is still not signed (ADR 0020 §3): the partition is written with the
image and the signature of the system covers the application, not a file of
the volume.

## Consequences

**Good**

* An update has a slot to return to, and the return happens without a person
  at the board: a watchdog, a panic or a refused signature is a boot in the
  old slot.
* The image of the system is still the only thing that is signed and the only
  thing an update writes; the data of the user and the settings in the NVS
  are not touched by an update.
* `/bin` is a real list of files of a partition, and replacing a program is a
  file in `progs.bin` and a flash, not a rebuild of the kernel and a boot that
  writes into MicroFS.
* The layout is decided once, before the port becomes a compatibility port,
  and it leaves 5.56 MiB of the 8 MB to the user - the largest volume these
  two slots and two programs allow.

**Bad**

* MicroFS shrinks from 5.94 MiB to 5.56 MiB (0.38 MiB), and a board of 1.0
  loses the old volume once when the table arrives.
* An application slot is 1.125 MiB and the image of 1.1 uses 84% of it; a 1.x
  build that grows by more than 177 KiB does not fit, and the compatibility
  rule of ADR 0018 is the promise that it does not have to.
* Two programs fit `progs` and no more at once: a third one replaces one of
  the two in a new image, or the layout changes (which is not promised).
* An update writes 1.125 MiB less the trailer, and `idf.py flash` writes the
  whole 0x21000 of `progs` even when nothing changed. The flash of the board
  is a finite resource; the update path of a person is the one that writes
  what changed.
* The rollback is not the anti-rollback of secure boot: a person with a flash
  writer can erase otadata, and the version floor in the NVS
  (`MICRO_OS_ANTIROLLBACK`) is a warning by default.
* The programs still share the kernel's address space (ADR 0018 §4), one at a
  time, in 8 KiB windows. Nothing of this ADR changes that.
* There is no update over the network; the cable is the update path of 1.1.

## What was built and measured

* `components/port_esp32/src/{progs_vfs.c,progs_format.c,ota.c}`,
  `tools/{progs_image.py,ota_usb.py}`, the table of §1 in `partitions.csv`
  and the bootloader option in `sdkconfig.defaults`.
* The image of the application is 993 664 bytes of the 1 175 552 bytes a slot
  leaves for an image and its trailer (181 888 bytes, 177.6 KiB, 15.4%, still
  free); the partition table is 3072 bytes; `progs.bin` is 135 168 bytes and
  holds `/bin/hello` (2 152 bytes at offset 0) and `/bin/cat` (1 920 bytes at
  offset 0x10000).
* The build refuses an image that fits but leaves less than 64 KiB after its
  trailer (`tools/check_image_fits.cmake`, `MIN_FREE`): a 1.x build that
  grows into the reserve is a decision about the layout, not a silent
  squeeze.
* `make idf-build`, `make bundle`, `tools/device-test.sh /dev/ttyACM0` and
  `DEVICE_OTA=1 tools/device-test.sh /dev/ttyACM0` on the board:
  * a flash of the bundle starts `ota_0`, the signature passes, `ls /bin`
    lists `cat` and `hello`, `/bin/hello` runs twice, and the shell confirms
    `ota_0`;
  * an update writes `ota_1`, the boot says `running from ota_1 (pending
    verify)` and then `the image of ota_1 is valid, the rollback is
    cancelled`; a second update returns to `ota_0`;
  * an image whose trailer does not match prints `the image is refused: the
    signature does not match`, marks `ota_1` invalid, resets by itself, and
    the board boots `ota_0` with `MicroOS shell ready`.
* The host tests include `tests/cases/test_progs.c` (the directory: magic,
  version, count, slot, offsets, names, duplicates and the partition bound);
  `make integrity` reads `progs.bin` of the bundle against the table.

## Alternatives

* **A factory partition plus one OTA slot.** 1.125 MB + 1.125 MB + programs +
  volume fits, but the factory slot cannot be updated and the board would
  have one image that is frozen: two equal OTA slots are what an update and a
  rollback need, and the ESP-IDF bootloader supports no factory at all.
* **Two 2 MB slots.** Works, but takes 1.75 MiB from MicroFS that the growth
  of a compatibility port does not need.
* **Two 1.1875 MiB slots.** 64 KiB more of growth and 64 KiB less volume; the
  1.125 MiB slot already leaves 177 KiB, and the user asked for the volume.
* **Two 1 MiB slots.** The image and its trailer leave only 50 KiB (5%) of a
  slot: a single compatibility fix would not fit.
* **The trailer in `progs`.** One trailer for two slots is a signature that
  does not say which image it covers, and an update of one slot would
  invalidate the other. The trailer belongs to the image.
* **A directory in the first slot of `progs`.** 64 KiB less volume for no
  program slot: the directory at the end and the files from offset 0 is the
  same alignment at 4 KiB of cost.
* **Map `progs` as instructions.** See §4: a fixed virtual address, a
  reserved mapping and a program linked for it. The copy is deterministic and
  already tested; the mapping is a later loader with its own price.
* **A bytecode or an ELF program.** ADR 0018 already refused both for this
  machine.

## Open items

1. An update over the network (HTTP) with the same slot, signature and
   rollback state, and the policy for a URL.
2. The flash mapping of `progs` as instructions, if the copy ever costs more
   than the 8 KiB of IRAM it saves.
3. A `progs` update that writes only the changed slot, and a host tool that
   keeps the directory at the end of the image.
4. The boot-chain check of ADR 0020 (secure boot v2 or a custom bootloader):
   otadata is still writable by anyone who can flash the board.
