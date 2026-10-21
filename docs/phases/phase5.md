# Phase 5 - The memory manager

The memory manager of the kernel gets slabs and a way for the programs to
ask for memory: `malloc`, `free` and `realloc` go through the ABI of the
system calls, like every other call.

## What is there

* **The slabs.** The small blocks (up to 512 bytes) come from slabs: one
  slab of 8 KB is cut into slots of 16, 32, 64, 128, 256 or 512 bytes, and
  the free slots are kept in a list. A thousand small blocks cost a few
  slabs, not a thousand calls of the allocator, and the fragments of the
  freed blocks are not lost.
* **The big blocks** (over 512 bytes) come straight from the allocator of
  the port, with the header of the manager in front of them.
* **The priority of the memory** works as before: the kernel and the
  drivers get the internal RAM, the applications get the PSRAM, and the
  reserve of 48 KB of the internal RAM is never given to an application.
* **The memory of the programs**: `malloc`, `free`, `realloc` and `calloc`
  in the library, `KSYS_MALLOC`, `KSYS_FREE` and `KSYS_REALLOC` in the
  ABI. The priority of the task decides where the memory comes from.
* The statistics know the number of the slabs and the free space inside
  them.

## The tests are about the properties

* **The data survives.** The blocks of the sizes of the corpus (the real
  numbers of the project) keep what was written into them, and the blocks
  are aligned.
* **The blocks do not overlap.** The pattern of one block never appears in
  another one.
* **The slabs are reused.** 256 blocks of 64 bytes fit into a few slabs,
  and after they are freed the same blocks are given out again - no new
  slab appears.
* **The priority of the small blocks** is the same as of the big ones: the
  user gets the PSRAM, the kernel gets the internal RAM.
* **The memory of the programs** keeps the data through a `realloc`, and
  the memory of `calloc` is empty.

## The mistakes that cost time

* **The PSRAM of the host was never reused.** The allocator of the host
  port kept the freed blocks in the list of the free blocks with the size
  zero, so the first-fit never gave them out again: every free leaked a
  little, and the pool was empty after a few thousand allocations. The
  tests of the manager failed with "the block is not ours" long before the
  memory of the system ran out. **The blocks are now merged with their
  neighbours when they are given back.**
* **The negative sizes of the corpus.** The sizes of the blocks were taken
  from the corpus as they are, and half of the corpus is negative. A
  negative size became a huge one, and the allocation failed.
* **The slabs leaked between the tests.** `kmm_init()` threw away the
  description of the slabs, but not their memory. Every test started with
  a little less memory. Now `kmm_init()` gives the slabs back.
* **The pool of the big blocks was forgotten.** The first version of the
  manager did not remember where a big block came from, so the statistics
  and the freeing were wrong.

## What is worth remembering

* The manager is the place where the fragmentation is decided. The slabs
  answer the small blocks, the big blocks stay as they are.
* The test harness is part of the system: the bug of the PSRAM of the host
  hid itself for a long time, because the memory of the host is endless.
* The statistics are the instrument of the manager: the number of the
  slabs and the free space inside them say more than the total.
