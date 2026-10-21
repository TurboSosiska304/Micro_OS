# Phase 2 - The filesystem, the VFS and the shell

The programs of the board get a tree of the directories, a filesystem and a
shell that works with the files.

## What is there

* **MicroFS** - the filesystem of the board, as it is described in
  [docs/memory.md](../memory.md): the sections DATA, META and INFO, the
  addresses are computed from the size of the volume, the META holds the map
  of the free blocks and the table of the inodes, the files grow in chains
  of blocks.
* **The VFS** - the filesystems are mounted at a point of the tree, the
  paths are resolved by the longest matching prefix.
* **The procfs** - `/proc` with `version`, `uptime`, `meminfo`, `tasks`,
  `modules`, `/proc/self` and a directory per task with a `status` file.
* **The devfs** - `/dev` with `tty0` (the console) and `null`.
* **The shell** - `help`, `uname`, `uptime`, `ps`, `echo`, `sleep`, `dmesg`,
  `free`, `clear`, `modules`, `memtest`, `i2c`, `abi`, `display`, `pwd`,
  `cd`, `ls`, `cat`, `write`, `mkdir`, `rm`, `df`, `mount`, `reboot`.
* **The console of the display** - the output of the shell is mirrored to the
  screen of the board, the font is 5x7.
* **The log of the boot** goes to `/log/start.log`, the console stays
  readable.

## The mistakes that cost time

* **The path of the root mount was refused.** The check "the point of the
  mount has to be a whole directory of the path" rejected every path below
  `/`. The root is the prefix of every path.
* **The list of a directory counted the entries of the previous call.** The
  buffer of the entries is static and was not cleared.
* **The virtual file did not end.** `/proc/uptime` was generated again and
  again, and `cat` looped forever. The offset of the descriptor says how
  much of the text was read already.
* **The image on the display was shifted.** The memory of the ST7789V2 is
  240x320, the glass of the 1.14" panel shows only a part of it. The
  offset is a variable now and can be changed while the board runs:
  `display <x> <y>`.
* **The shell did not repeat the input.** The typed line appeared only
  after the Enter, now every character is repeated at once and the
  backspace erases it.
* **The flash of the board is full.** There is no room for the crash dump
  any more, it is switched off.

## What is worth remembering

* The filesystem talks to a **block device**, not to the flash. The tests
  use a volume in the memory, the board uses the partition of the flash.
* The directory is a chain of blocks as well, so a directory is not limited
  to one block.
