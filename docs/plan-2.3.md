# 2.3 "Pompeii": the device-fault slice

* Status: **implemented on `feature/fault-inject`**; the parser fuzzing and the
  allocator campaign are not part of this slice.
* Base: `2b077f7b` (the 2.2 line).
* Tests: `tests/cases/test_blockdev_fault.c` and
  `tests/cases/test_mfs_fault.c`.

## The injector

`kernel/blockdev_fault.h` is visible only when `K_BLOCKDEV_TEST` is defined.
`tests/src/blockdev_fault.c` is compiled only into `microos_tests`, so neither
the firmware nor a kernel library contains an injection control.

The bus wraps an ordinary `struct k_blockdev`. Every `read`, `write`, `erase`,
`sync`, `flush` and write-door call receives a one-based operation number; the
trace also records its kind and first sector. A plan selects one of:

| Mode | Effect |
|---|---|
| `ERROR` | returns `KERR_IO`, `KERR_NOSPC` or `KERR_TIMEOUT` at the selected operation |
| `DROP_WRITE` | returns success without writing; the next durability barrier fails until reboot |
| `REORDER` | holds one write and applies it after the selected second write |
| `POWER_CUT` | completes the selected operation, then stops the bus; reboot restores only the stable image |

The medium has a volatile and a stable image. A successful `sync` or `flush`
copies volatile to stable. A reorder held across a barrier is drained before
the copy, so the injector does not break the barrier it is testing. A dropped
write cannot be detected by MicroFS data blocks (they have no checksum and are
not journaled), so the model marks the medium unproven and refuses the next
barrier; the write itself still returns success as requested.

## Coverage

Each trial starts from the same formatted volume, runs one real filesystem
operation, discards volatile state, mounts again, requires a clean `mfs_check`,
checks the keeper and the operation's old/whole/absent states, and creates,
`fsync`s, reads and removes a canary. The canary is the "machine is alive"
check after every point.

For a baseline of `N` operations, `W` writes and `P` reorder pairs, the sweep
runs `3*N` error points (`EIO`, `ENOSPC`, `TIMEOUT`), `N` power cuts, `W`
dropped writes and `P` reorder pairs. Reorder pairs are adjacent writes plus a
write and the first write after its next durability barrier.

| Scenario | Baseline ops | Writes | Reorder pairs | Points | Passed |
|---|---:|---:|---:|---:|---:|
| create | 55 | 12 | 19 | 251 | 251 |
| append past EOF | 542 | 207 | 310 | 2 685 | 2 685 |
| rename with replacement | 72 | 18 | 31 | 337 | 337 |
| unlink | 65 | 15 | 25 | 300 | 300 |
| `fsync` | 544 | 208 | 311 | 2 695 | 2 695 |
| mount | 23 | 0 | 0 | 92 | 92 |
| **Total** | **1 301** | **460** | **696** | **6 360** | **6 360** |

The total is 3 903 error points, 1 301 power cuts, 460 dropped writes and 696
reordered pairs.

## Defects found and fixed

1. **The block cache answered from a box the medium had refused.** A failed
   write-back flush marked the dirty run invalid but left it in the lookup
   index. A later read returned those rejected bytes without asking the
   medium. `cache_flush_run()` now removes every dropped box from the index;
   `test_blockdev_cache.c` reproduces the old `memcmp` failure.
2. **The journal committed before proving its slots and commit header.**
   `journal_flush()` validated a slot only while applying it, so a stale slot
   header could redirect a new image to an unrelated home and a later bad slot
   could leave earlier homes replaced. It also did not read the commit header
   back. All slots and their map checksum are now checked before commit, the
   committed header is read back before any home is touched, and replay can
   roll back a bad image instead of refusing the mount.
3. **`fsync` published the inode and bitmap in different transactions.** A cut
   between them left 49 blocks used by an object but marked free. The public
   `mfs_fsync()` now wraps both in one transaction and runs the device flush
   only after that transaction is durable.
4. **Chain release ignored a read error.** `file_release_chain()` ignored the
   second `chain_next()` result and could commit a partially released chain.
   The error now aborts the operation.
5. **Rename treated an unreadable destination as an absent one.** Any
   `path_lookup(to)` error except `MFS_ENOENT` now aborts; previously an EIO
   could add a duplicate destination name.

## Gates

* `make test`: **pass**, 6/6 CTest tests; 80 116 unit checks, 0 failures.
* `make test-smp`: the first full run exposed an unrelated intermittent
  `test_tty.c:373` state assertion; an isolated unit rerun passed. The final
  full gate result is recorded below after the branch gate run.
* `make integrity`: **pass**, 30/30 checks.
* `make x86-build`: **pass**.
* `make x86-smoke`: pending a quiet host (no other QEMU, load average below 2).

## Before the night of fuzzing

* Add allocator-failure injection at the `kmm_alloc`/`kmm_free` boundaries and
  run the same mount/check/canary recovery campaign.
* Add the parser corpora and deterministic seeds for MicroFS images, ext2
  images, MOX1, network packets and shell lines; run them under ASan with a
  fixed time and memory budget.
* Turn every panic, hang, leaked lock and accepted malformed image into a
  minimized regression. Keep page cache and GPU work in their own lanes.
