/*
 * Micro OS — the test-only fault bus of the block layer.
 *
 * The implementation lives with the host tests, not in microos_kernel: a
 * production build cannot name an injection control even by accident.
 */
#include "kernel/blockdev_fault.h"

#ifdef K_BLOCKDEV_TEST

#include "kernel/errno.h"

#include <stdlib.h>
#include <string.h>

static bool span_ok(const struct k_blockdev_fault *fault, k_u32_t sector, k_size_t count)
{
    return sector < fault->medium->sectors && count <= fault->medium->sectors - sector;
}

static k_u32_t operation_begin(struct k_blockdev_fault *fault, k_u32_t kind, k_u32_t sector)
{
    k_u32_t operation;

    if (fault->stats.operations < K_BLOCKDEV_FAULT_TRACE_MAX) {
        fault->trace[fault->stats.operations] = (k_u8_t)kind;
        fault->trace_sector[fault->stats.operations] = sector;
    } else {
        fault->stats.trace_overflow = true;
    }

    fault->stats.operations++;

    switch (kind) {
    case K_BLOCKDEV_FAULT_OP_READ:
        fault->stats.reads++;
        break;
    case K_BLOCKDEV_FAULT_OP_WRITE:
        fault->stats.writes++;
        break;
    case K_BLOCKDEV_FAULT_OP_ERASE:
        fault->stats.erases++;
        break;
    case K_BLOCKDEV_FAULT_OP_SYNC:
        fault->stats.syncs++;
        break;
    case K_BLOCKDEV_FAULT_OP_FLUSH:
        fault->stats.flushes++;
        break;
    default:
        break;
    }

    operation = fault->stats.operations;

    if ((fault->plan.modes & K_BLOCKDEV_FAULT_ERROR) != 0u && operation == fault->plan.at) {
        fault->stats.injected++;
        return 0;
    }

    return operation;
}

static int operation_error(const struct k_blockdev_fault *fault, k_u32_t operation)
{
    if (operation == 0) {
        return fault->plan.error != 0 ? fault->plan.error : KERR_IO;
    }

    return KERR_NONE;
}

static void operation_cut(struct k_blockdev_fault *fault, k_u32_t operation)
{
    if ((fault->plan.modes & K_BLOCKDEV_FAULT_POWER_CUT) != 0u && operation == fault->plan.at) {
        fault->stats.injected++;
        fault->power_lost = true;
    }
}

static void held_free(struct k_blockdev_fault *fault)
{
    free(fault->held_data);
    fault->held_data = NULL;
    fault->held = false;
    fault->held_count = 0;
}

static int held_apply(struct k_blockdev_fault *fault)
{
    int rc;

    if (!fault->held) {
        return KERR_NONE;
    }

    rc = fault->medium->ops->write(fault->medium->ctx, fault->held_sector, fault->held_data,
                                   fault->held_count);
    if (rc == KERR_NONE) {
        fault->stats.reordered++;
    }

    held_free(fault);

    return rc;
}

static int fault_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct k_blockdev_fault *fault = ctx;
    k_u32_t operation;
    int rc;

    if (buf == NULL || !span_ok(fault, sector, count)) {
        return KERR_INVAL;
    }
    if (fault->power_lost) {
        return KERR_TIMEOUT;
    }

    operation = operation_begin(fault, K_BLOCKDEV_FAULT_OP_READ, sector);
    rc = operation_error(fault, operation);
    if (rc != KERR_NONE) {
        return rc;
    }

    rc = fault->medium->ops->read(fault->medium->ctx, sector, buf, count);
    operation_cut(fault, operation);

    return rc;
}

static int fault_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct k_blockdev_fault *fault = ctx;
    k_u32_t operation;
    int rc;

    if (buf == NULL || !span_ok(fault, sector, count)) {
        return KERR_INVAL;
    }
    if (fault->power_lost) {
        return KERR_TIMEOUT;
    }

    operation = operation_begin(fault, K_BLOCKDEV_FAULT_OP_WRITE, sector);
    rc = operation_error(fault, operation);
    if (rc != KERR_NONE) {
        return rc;
    }

    if ((fault->plan.modes & K_BLOCKDEV_FAULT_DROP_WRITE) != 0u && operation == fault->plan.at) {
        fault->stats.injected++;
        fault->stats.dropped++;
        fault->write_lost = true;
        operation_cut(fault, operation);
        return KERR_NONE;
    }

    if ((fault->plan.modes & K_BLOCKDEV_FAULT_REORDER) != 0u) {
        if (operation == fault->plan.first) {
            k_size_t bytes = count * fault->medium->sector_size;

            if (fault->held) {
                return KERR_BUSY;
            }

            fault->held_data = (k_u8_t *)malloc(bytes);
            if (fault->held_data == NULL) {
                return KERR_NOMEM;
            }

            memcpy(fault->held_data, buf, bytes);
            fault->held_sector = sector;
            fault->held_count = count;
            fault->held = true;
            operation_cut(fault, operation);
            return KERR_NONE;
        }

        if (operation == fault->plan.second) {
            rc = fault->medium->ops->write(fault->medium->ctx, sector, buf, count);
            if (rc == KERR_NONE) {
                rc = held_apply(fault);
            }
            operation_cut(fault, operation);
            return rc;
        }
    }

    rc = fault->medium->ops->write(fault->medium->ctx, sector, buf, count);
    operation_cut(fault, operation);

    return rc;
}

static int fault_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    struct k_blockdev_fault *fault = ctx;
    k_u32_t operation;
    int rc;

    if (!span_ok(fault, sector, count)) {
        return KERR_INVAL;
    }
    if (fault->power_lost) {
        return KERR_TIMEOUT;
    }

    operation = operation_begin(fault, K_BLOCKDEV_FAULT_OP_ERASE, sector);
    rc = operation_error(fault, operation);
    if (rc != KERR_NONE) {
        return rc;
    }

    rc = fault->medium->ops->erase(fault->medium->ctx, sector, count);
    operation_cut(fault, operation);

    return rc;
}

static int barrier_apply(struct k_blockdev_fault *fault, k_u32_t kind)
{
    k_u32_t operation;
    int rc;

    if (fault->power_lost) {
        return KERR_TIMEOUT;
    }

    operation = operation_begin(fault, kind, 0);
    rc = operation_error(fault, operation);
    if (rc != KERR_NONE) {
        return rc;
    }

    /* The write lied successfully, but the medium is no longer proven
     * durable. A barrier must not publish the surrounding transaction over a
     * block the device knows it lost. A reboot clears the condition with the
     * volatile image. */
    if (fault->write_lost) {
        return KERR_IO;
    }

    /* The barrier drains a reordered write before it commits the image. */
    rc = held_apply(fault);
    if (rc != KERR_NONE) {
        return rc;
    }

    if (kind == K_BLOCKDEV_FAULT_OP_FLUSH && fault->medium->ops->flush != NULL) {
        rc = fault->medium->ops->flush(fault->medium->ctx);
    } else if (fault->medium->ops->sync != NULL) {
        rc = fault->medium->ops->sync(fault->medium->ctx);
    }

    if (rc == KERR_NONE) {
        memcpy(fault->stable_blocks, fault->volatile_blocks, fault->bytes);
        fault->stats.commits++;
    }

    operation_cut(fault, operation);

    return rc;
}

static int fault_sync(void *ctx)
{
    return barrier_apply(ctx, K_BLOCKDEV_FAULT_OP_SYNC);
}

static int fault_flush(void *ctx)
{
    return barrier_apply(ctx, K_BLOCKDEV_FAULT_OP_FLUSH);
}

static int fault_writes(void *ctx, bool open)
{
    struct k_blockdev_fault *fault = ctx;
    k_u32_t operation;
    int rc;

    if (fault->power_lost) {
        return KERR_TIMEOUT;
    }

    operation = operation_begin(fault, K_BLOCKDEV_FAULT_OP_WRITES, 0);
    rc = operation_error(fault, operation);
    if (rc != KERR_NONE) {
        return rc;
    }

    rc = fault->medium->ops->writes(fault->medium->ctx, open);
    operation_cut(fault, operation);

    return rc;
}

int k_blockdev_fault_open(struct k_blockdev_fault *fault, const struct k_blockdev *medium,
                          k_u8_t *volatile_blocks, k_u8_t *stable_blocks, k_size_t bytes)
{
    k_size_t need;

    if (fault == NULL || medium == NULL || volatile_blocks == NULL || stable_blocks == NULL ||
        medium->ops == NULL || medium->ops->read == NULL || medium->ops->write == NULL ||
        medium->sector_size == 0u || medium->sectors == 0u) {
        return -1;
    }

    need = (k_size_t)medium->sectors * medium->sector_size;
    if (bytes < need) {
        return -1;
    }

    memset(fault, 0, sizeof(*fault));
    fault->medium = medium;
    fault->volatile_blocks = volatile_blocks;
    fault->stable_blocks = stable_blocks;
    fault->bytes = need;
    fault->dev = *medium;
    fault->dev.ctx = fault;
    fault->ops.read = fault_read;
    fault->ops.write = fault_write;
    fault->ops.erase = medium->ops->erase != NULL ? fault_erase : NULL;
    fault->ops.sync = fault_sync;
    fault->ops.flush = fault_flush;
    fault->ops.writes = medium->ops->writes != NULL ? fault_writes : NULL;
    fault->dev.ops = &fault->ops;

    return 0;
}

void k_blockdev_fault_close(struct k_blockdev_fault *fault)
{
    if (fault != NULL) {
        held_free(fault);
    }
}

int k_blockdev_fault_arm(struct k_blockdev_fault *fault, const struct k_blockdev_fault_plan *plan)
{
    if (fault == NULL || plan == NULL) {
        return -1;
    }

    if ((plan->modes & K_BLOCKDEV_FAULT_REORDER) != 0u &&
        (plan->first == 0u || plan->second <= plan->first)) {
        return -1;
    }

    if ((plan->modes & K_BLOCKDEV_FAULT_ERROR) != 0u && plan->at == 0u) {
        return -1;
    }

    if ((plan->modes & (K_BLOCKDEV_FAULT_DROP_WRITE | K_BLOCKDEV_FAULT_POWER_CUT)) != 0u &&
        plan->at == 0u) {
        return -1;
    }

    held_free(fault);
    memset(&fault->stats, 0, sizeof(fault->stats));
    memset(fault->trace, 0, sizeof(fault->trace));
    memset(fault->trace_sector, 0, sizeof(fault->trace_sector));
    fault->plan = *plan;
    fault->power_lost = false;
    fault->write_lost = false;

    return 0;
}

void k_blockdev_fault_reboot(struct k_blockdev_fault *fault)
{
    struct k_blockdev_fault_plan plan = {0};

    if (fault == NULL) {
        return;
    }

    memcpy(fault->volatile_blocks, fault->stable_blocks, fault->bytes);
    (void)k_blockdev_fault_arm(fault, &plan);
}

k_u32_t k_blockdev_fault_kind_at(const struct k_blockdev_fault *fault, k_u32_t operation)
{
    if (fault == NULL || operation == 0u || operation > fault->stats.operations ||
        operation > K_BLOCKDEV_FAULT_TRACE_MAX) {
        return 0;
    }

    return fault->trace[operation - 1u];
}

k_u32_t k_blockdev_fault_sector_at(const struct k_blockdev_fault *fault, k_u32_t operation)
{
    if (fault == NULL || operation == 0u || operation > fault->stats.operations ||
        operation > K_BLOCKDEV_FAULT_TRACE_MAX) {
        return 0;
    }

    return fault->trace_sector[operation - 1u];
}

#endif /* K_BLOCKDEV_TEST */
