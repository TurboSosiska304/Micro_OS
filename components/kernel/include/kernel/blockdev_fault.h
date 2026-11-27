/*
 * Micro OS — deterministic faults for the block layer tests.
 *
 * This header is deliberately invisible to the firmware and to the kernel
 * libraries. A test target defines K_BLOCKDEV_TEST and compiles the small bus
 * in tests/src/blockdev_fault.c; the shipping block devices have no fault API.
 */
#ifndef MICRO_OS_KERNEL_BLOCKDEV_FAULT_H
#define MICRO_OS_KERNEL_BLOCKDEV_FAULT_H

#include "kernel/blockdev.h"

#ifdef K_BLOCKDEV_TEST

#include "kernel/types.h"

/* The bits of a fault plan. A plan may combine them, although the filesystem
 * sweeps use one at a time so that a failure names its cause. */
#define K_BLOCKDEV_FAULT_ERROR 0x01u
#define K_BLOCKDEV_FAULT_DROP_WRITE 0x02u
#define K_BLOCKDEV_FAULT_REORDER 0x04u
#define K_BLOCKDEV_FAULT_POWER_CUT 0x08u

/* The kind of one call, as recorded in the trace of a fault device. */
#define K_BLOCKDEV_FAULT_OP_READ 0x01u
#define K_BLOCKDEV_FAULT_OP_WRITE 0x02u
#define K_BLOCKDEV_FAULT_OP_ERASE 0x04u
#define K_BLOCKDEV_FAULT_OP_SYNC 0x08u
#define K_BLOCKDEV_FAULT_OP_FLUSH 0x10u
#define K_BLOCKDEV_FAULT_OP_WRITES 0x20u

/* A filesystem session is small, but the cap makes a forgotten loop a test
 * failure instead of an overwrite of the trace. */
#define K_BLOCKDEV_FAULT_TRACE_MAX 4096u

struct k_blockdev_fault_plan {
    k_u32_t modes;
    k_u32_t at;    /* the one-based number of the operation */
    int error;     /* KERR_IO, KERR_NOSPC or KERR_TIMEOUT */
    k_u32_t first; /* the one-based numbers of two writes to reorder */
    k_u32_t second;
};

struct k_blockdev_fault_stats {
    k_u32_t operations;
    k_u32_t reads;
    k_u32_t writes;
    k_u32_t erases;
    k_u32_t syncs;
    k_u32_t flushes;
    k_u32_t injected;
    k_u32_t dropped;
    k_u32_t reordered;
    k_u32_t commits;
    bool trace_overflow;
};

/*
 * A bus between a filesystem and a volatile memory device. The medium sees
 * volatile_blocks; a successful sync or flush copies that image to
 * stable_blocks. A reboot copies stable back to volatile, the way a power cut
 * discards the cache of a disk.
 *
 * The first write of a reorder pair is held until the second arrives. A sync
 * drains the held write before committing, because a device that reordered
 * writes across its own durability barrier would make fsync a lie. A dropped
 * write returns success but makes the medium unproven: the next sync or flush
 * fails until a reboot, so no surrounding transaction is published over it.
 */
struct k_blockdev_fault {
    struct k_blockdev dev;
    struct k_blockdev_ops ops;
    const struct k_blockdev *medium;
    k_u8_t *volatile_blocks;
    k_u8_t *stable_blocks;
    k_size_t bytes;
    struct k_blockdev_fault_plan plan;
    struct k_blockdev_fault_stats stats;
    k_u8_t trace[K_BLOCKDEV_FAULT_TRACE_MAX];
    k_u32_t trace_sector[K_BLOCKDEV_FAULT_TRACE_MAX];
    bool power_lost;
    bool write_lost;
    bool held;
    k_u32_t held_sector;
    k_size_t held_count;
    k_u8_t *held_data;
};

/* Prepare the bus. The medium's context must use volatile_blocks. */
int k_blockdev_fault_open(struct k_blockdev_fault *fault, const struct k_blockdev *medium,
                          k_u8_t *volatile_blocks, k_u8_t *stable_blocks, k_size_t bytes);

/* Give back the buffer of a held write. The images belong to the caller. */
void k_blockdev_fault_close(struct k_blockdev_fault *fault);

/* Start a session with a plan and empty counters. */
int k_blockdev_fault_arm(struct k_blockdev_fault *fault, const struct k_blockdev_fault_plan *plan);

/* Simulate a reboot: the stable image becomes the volatile one and the bus is
 * disarmed. The filesystem must be mounted again to forget its old memory. */
void k_blockdev_fault_reboot(struct k_blockdev_fault *fault);

/* The kind of a recorded operation, or zero when the number is out of range. */
k_u32_t k_blockdev_fault_kind_at(const struct k_blockdev_fault *fault, k_u32_t operation);

/* The first sector of a recorded operation, or zero for a barrier. */
k_u32_t k_blockdev_fault_sector_at(const struct k_blockdev_fault *fault, k_u32_t operation);

#endif /* K_BLOCKDEV_TEST */

#endif /* MICRO_OS_KERNEL_BLOCKDEV_FAULT_H */
