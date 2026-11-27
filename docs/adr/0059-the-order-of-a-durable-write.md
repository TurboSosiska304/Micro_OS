# ADR 0059 - the order of a durable write

* Status: accepted, implemented
* Date: 2026-11-27
* Related: ADR 0047 (a file that survives a cut), ADR 0009 (the format and its
  check), `docs/debts.md`

## Context

ADR 0047 gave MicroFS a metadata redo journal: a committed transaction is
replayed at mount, a draft file is removed, and `fsck` finds the volume
consistent. The promise was proven for a large file write and a rename, but the
write order was not made explicit as barriers between phases, and it was not
proven for every path that changes the volume (create, unlink/rmdir, write past
end of file). A write-back block cache also made the order inside one flush
undefined: journal slots, data blocks and a commit header could reach the medium
in cache-box order, not in write order.

The release also needs `fsync`: after the call returns, the data and the
metadata of the file are on the medium, not only in a volatile cache.

## Decision

### 1. The order and the barriers

Every operation that changes a volume follows four phases with explicit
barriers between them:

1. **journal** - write the metadata images into journal slots (redo log). The
   home blocks are not touched.
2. **data** - write file data and newly allocated chain blocks to their home
   locations. A new block is on the medium before a chain link points to it.
3. **metadata/inode** - the metadata of the transaction (inode, bitmap,
   directory blocks, volume header) is staged in the journal as slot images.
4. **commit** - a barrier brings journal slots and data to the medium, the
   journal header is written with `COMMITTED`, and a second barrier makes the
   commit header durable before home blocks are applied. A checkpoint barrier
   follows the home apply; the empty header needs no flush because replay is
   idempotent.

The barriers are `dev_sync()` calls. On a write-through device a barrier is a
no-op beyond the device; on a write-back cache it flushes all dirty sectors, so
the barrier is a total order point. The pre-commit barrier covers phases 1-3 and
any data written before commit; the commit header is written only after it.

Data of a file is not journaled (ADR 0047 §4). It is protected by the PENDING
flag: a file opened for writing is marked PENDING before data changes, and the
close publishes inode, size and bitmap in one committed transaction. A cut before
publication leaves PENDING on the medium and mount recovery removes the draft.

### 2. Replay and checkpoint

Mount reads the journal header. A valid `COMMITTED` transaction is verified
(slot sequence and CRC, image CRC, home bounds) and replayed idempotently to home
blocks, then synced and checkpointed to `EMPTY`. An uncommitted, damaged or
mismatched transaction is ignored and the home blocks keep the state of the last
checkpoint. After replay, mount runs pending-draft recovery and the volume passes
`fsck` without `-y`.

### 3. fsync

`vfs_fsync`/`ksys_fsync` bring an open file to the medium. MFS publishes the
current inode (clearing PENDING), syncs the bitmap/journal, syncs the blockdev
cache, and issues the blockdev `flush` operation. The blockdev `flush` is the
media guarantee: it returns success only when the device reports the volatile
write cache flushed, and `KERR_NOTSUP` when the medium cannot make that promise.

| medium | command | support |
|---|---:|---|
| ATA PIO (IDE) | `FLUSH CACHE` 0xE7 | yes |
| AHCI/SATA | `FLUSH CACHE EXT` 0xEA | yes |
| virtio-blk | `VIRTIO_BLK_T_FLUSH` | yes when feature present; no-op success when device has no volatile cache |
| USB mass storage (BOT) | SCSI `SYNCHRONIZE CACHE(10)` 0x35 | yes |
| NVMe | Flush | yes |
| ramdisk / memory | none | success (write-through) |

A device with a volatile cache and no flush command must answer `KERR_NOTSUP`,
never pretend success.

## Consequences

**Good**

* The write order is explicit and guarded by barriers; write-back caches cannot
  reorder commit before journal/data.
* Create, rename, unlink/rmdir and write-past-EOF are covered by cut-at-every-
  write tests: mount, replay and `fsck` without `-y` pass, and files are whole or
  absent.
* `fsync` gives a real medium guarantee and refuses honestly when unsupported.

**Bad**

* Barriers flush the whole write-back cache; they are correct but coarser than a
  per-block ordered write. The cost is acceptable for the metadata-heavy journal
  workload and can be narrowed later with tagged flushes.
* File data is still not journaled; a modified-in-place file removed by PENDING
  recovery loses its old content (ADR 0047 consequence, unchanged).
* `fsync` publishes the file's current metadata; it is a durability point, not a
  byte-range snapshot interface.
