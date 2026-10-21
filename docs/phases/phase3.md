# Phase 3 - The ABI of the system calls

The interface between the kernel and the programs. The numbers of the calls
are a **contract**: they are written down in
[docs/syscall-abi.md](../syscall-abi.md) and repeated in a test, so that a
change is noticed at once.

## What is there

* The table of the calls: `exit`, `read`, `write`, `open`, `close`, `stat`,
  `lseek`, `unlink`, `mkdir`, `readdir`, `getpid`, `getcwd`, `ioctl`,
  `yield`, `sleep`, `uptime`, `reboot`.
* The wrappers with the checked arguments.
* Every task has its own table of the open files.

## The tests are about the properties

The tests do not check that "something" works, they check that the same data
gives the same result no matter how the program breaks it into pieces. The
corpus lives in `tests/corpus/vectors.txt`: 512 real numbers with the boundary
values mixed in (0, 1, 255, 65535, 2147483647 and so on).

* **The round trip.** The corpus is written through the calls in pieces of 1,
  3, 7, 64, 512, 1024 and 4096 bytes. Every time the same bytes come back
  and their sum is the same.
* **The alignment.** The same data, written from an aligned buffer and from
  one that is one byte further, gives the same checksum.
* **The contract.** The table of the numbers is repeated in the test.
* **The errors.** The same error, no matter how the call was made.
* **The agreement.** The file that the calls write is the file that the
  filesystem sees.

## The mistakes that cost time

* **The arguments were 32 bits wide.** A pointer of the 64 bit host was cut
  in half and the program died with a segmentation error. The width of the
  arguments is now the width of a pointer of the target: 32 bits on the
  board, 64 on the host. The numbers of the calls are the same everywhere,
  the width belongs to the target, like in the real syscall interface.
* **The kernel never called `kmm_init()` and `ksys_init()` at the boot.**
  The call of the scheduler had been renamed, the replacement of the source
  did not apply, and the file was written unchanged. The host tests hid it,
  because they called the initialisers by hand. **The device found it.**

## What is worth remembering

* The width of the arguments follows the target. The kernel of the board
  has 32 bit registers, so the arguments are 32 bits there.
* The tests of the ABI are the tests of the *properties*, not of the
  results. A test that only checks that the result is not zero would have
  passed with the truncated pointers.
