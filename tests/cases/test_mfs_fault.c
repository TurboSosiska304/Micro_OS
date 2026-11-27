/*
 * Micro OS — device faults through the real MicroFS journal.
 *
 * The journal cut-sweep loses every write after a point. This test asks a
 * different question: one command fails, lies about a write, arrives out of
 * order, or is followed by power loss. After every point the volatile image is
 * discarded, the volume is mounted again, fsck must be clean, the keeper must
 * be whole, the object of the operation must be whole or absent (or still in
 * its old state), and a new file must survive a write/fsync/read/unlink cycle.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/blockdev_fault.h"
#include "kernel/errno.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

#define FAULT_VOLUME_BLOCKS 512u
#define FAULT_VOLUME_BYTES ((k_size_t)FAULT_VOLUME_BLOCKS * MFS_BLOCK_SIZE)
#define FAULT_CHUNK 1024u
#define FAULT_KEEP_BYTES 32u
#define FAULT_GROW_OLD (32u + 48u * 1024u)
#define FAULT_SRC_BYTES 37u
#define FAULT_DST_BYTES 53u
#define FAULT_VICTIM_BYTES 1024u
#define FAULT_SYNC_BYTES (48u * 1024u)

static struct mfs_ram_device g_ram;
static struct k_blockdev g_medium;
static struct k_blockdev_fault g_fault;
static k_u8_t *g_stable;
static k_u8_t *g_base;
static k_u32_t g_points;
static k_u32_t g_passed;

struct scenario_context {
    bool mounted;
    bool fsynced;
};

struct scenario {
    const char *name;
    bool (*run)(struct scenario_context *ctx);
    bool (*verify)(const struct scenario_context *ctx);
};

struct sweep {
    k_u32_t points;
    k_u32_t passed;
    k_u32_t reported;
};

struct reorder_pair {
    k_u32_t first;
    k_u32_t second;
};

static void pattern(k_u8_t *out, k_size_t offset, k_size_t size, k_u8_t seed)
{
    k_size_t i;

    for (i = 0; i < size; i++) {
        out[i] = (k_u8_t)((offset + i) * 7u + seed);
    }
}

static bool write_pattern(struct mfs_file *file, k_size_t offset, k_size_t size, k_u8_t seed)
{
    k_u8_t chunk[FAULT_CHUNK];

    while (size != 0u) {
        k_size_t part = size < sizeof(chunk) ? size : sizeof(chunk);
        int rc;

        pattern(chunk, offset, part, seed);
        rc = mfs_write(file, chunk, part);
        if (rc != (int)part) {
            return false;
        }

        offset += part;
        size -= part;
    }

    return true;
}

static bool make_file(const char *path, k_size_t size, k_u8_t seed)
{
    struct mfs_file file;

    if (mfs_create(path) != MFS_OK) {
        return false;
    }

    if (mfs_open(path, MFS_O_WRONLY, &file) != MFS_OK) {
        return false;
    }

    if (!write_pattern(&file, 0, size, seed)) {
        (void)mfs_close(&file);
        return false;
    }

    return mfs_close(&file) == MFS_OK;
}

static bool file_matches(const char *path, k_size_t size, k_u8_t seed)
{
    struct mfs_file file;
    struct mfs_inode inode;
    k_u8_t chunk[FAULT_CHUNK];
    k_u8_t want[FAULT_CHUNK];
    k_size_t offset = 0;

    if (mfs_stat(path, &inode, NULL) != MFS_OK || inode.size != size) {
        return false;
    }

    if (size == 0u) {
        return true;
    }

    if (mfs_open(path, MFS_O_RDONLY, &file) != MFS_OK) {
        return false;
    }

    while (offset < size) {
        k_size_t part = size - offset < sizeof(chunk) ? size - offset : sizeof(chunk);
        int rc = mfs_read(&file, chunk, part);

        if (rc != (int)part) {
            (void)mfs_close(&file);
            return false;
        }

        pattern(want, offset, part, seed);
        if (memcmp(chunk, want, part) != 0) {
            (void)mfs_close(&file);
            return false;
        }

        offset += part;
    }

    return mfs_close(&file) == MFS_OK;
}

static bool file_absent(const char *path)
{
    struct mfs_inode inode;

    return mfs_stat(path, &inode, NULL) == MFS_ENOENT;
}

static bool keeper_whole(void)
{
    return file_matches("/keep.txt", FAULT_KEEP_BYTES, 11u);
}

static bool scenario_create(struct scenario_context *ctx)
{
    (void)ctx;
    return mfs_create("/new.txt") == MFS_OK;
}

static bool verify_create(const struct scenario_context *ctx)
{
    struct mfs_inode inode;
    (void)ctx;

    if (file_absent("/new.txt")) {
        return true;
    }

    return mfs_stat("/new.txt", &inode, NULL) == MFS_OK && inode.size == 0u;
}

static bool scenario_grow(struct scenario_context *ctx)
{
    struct mfs_file file;

    (void)ctx;

    if (mfs_open("/grow.txt", MFS_O_WRONLY | MFS_O_APPEND, &file) != MFS_OK) {
        return false;
    }

    if (!write_pattern(&file, FAULT_KEEP_BYTES, FAULT_GROW_OLD - FAULT_KEEP_BYTES, 22u)) {
        return false;
    }

    return mfs_close(&file) == MFS_OK;
}

static bool verify_grow(const struct scenario_context *ctx)
{
    (void)ctx;
    return file_absent("/grow.txt") || file_matches("/grow.txt", FAULT_KEEP_BYTES, 22u) ||
           file_matches("/grow.txt", FAULT_GROW_OLD, 22u);
}

static bool scenario_rename(struct scenario_context *ctx)
{
    (void)ctx;
    return mfs_rename("/src.txt", "/dst.txt") == MFS_OK;
}

static bool verify_rename(const struct scenario_context *ctx)
{
    bool old_names;
    bool new_names;
    (void)ctx;

    old_names = file_matches("/src.txt", FAULT_SRC_BYTES, 33u) &&
                file_matches("/dst.txt", FAULT_DST_BYTES, 44u);
    new_names = file_absent("/src.txt") && file_matches("/dst.txt", FAULT_SRC_BYTES, 33u);

    return old_names || new_names;
}

static bool scenario_unlink(struct scenario_context *ctx)
{
    (void)ctx;
    return mfs_unlink("/victim.txt") == MFS_OK;
}

static bool verify_unlink(const struct scenario_context *ctx)
{
    (void)ctx;
    return file_absent("/victim.txt") || file_matches("/victim.txt", FAULT_VICTIM_BYTES, 55u);
}

static bool scenario_fsync(struct scenario_context *ctx)
{
    struct mfs_file file;
    int rc;

    if (mfs_open("/sync.txt", MFS_O_WRONLY, &file) != MFS_OK) {
        return false;
    }

    if (!write_pattern(&file, 0, FAULT_SYNC_BYTES, 66u)) {
        return false;
    }

    rc = mfs_fsync(&file);
    if (rc != MFS_OK) {
        return false;
    }

    ctx->fsynced = true;
    if (!g_fault.power_lost) {
        (void)mfs_close(&file);
    }

    return true;
}

static bool verify_fsync(const struct scenario_context *ctx)
{
    if (ctx->fsynced) {
        return file_matches("/sync.txt", FAULT_SYNC_BYTES, 66u);
    }

    return file_absent("/sync.txt") || file_matches("/sync.txt", 0u, 66u) ||
           file_matches("/sync.txt", FAULT_SYNC_BYTES, 66u);
}

static bool scenario_mount(struct scenario_context *ctx)
{
    (void)ctx;
    return true;
}

static bool verify_mount(const struct scenario_context *ctx)
{
    (void)ctx;
    return true;
}

static const struct scenario g_scenarios[] = {
    {"create", scenario_create, verify_create},
    {"append-past-eof", scenario_grow, verify_grow},
    {"rename-replace", scenario_rename, verify_rename},
    {"unlink", scenario_unlink, verify_unlink},
    {"fsync", scenario_fsync, verify_fsync},
    {"mount", scenario_mount, verify_mount},
};

static void restore_base(void)
{
    memcpy(g_ram.data, g_base, FAULT_VOLUME_BYTES);
    memcpy(g_stable, g_base, FAULT_VOLUME_BYTES);
}

static bool run_scenario(const struct scenario *scenario, struct scenario_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    if (mfs_mount(&g_fault.dev) != MFS_OK) {
        return false;
    }

    ctx->mounted = true;
    if (g_fault.power_lost || !scenario->run(ctx)) {
        return false;
    }

    return !g_fault.power_lost;
}

static bool recovery_canary(void)
{
    static const char text[] = "alive";
    struct mfs_file file;
    char back[sizeof(text)];
    struct mfs_inode inode;

    if (mfs_create("/alive.tmp") != MFS_OK) {
        return false;
    }
    if (mfs_open("/alive.tmp", MFS_O_WRONLY, &file) != MFS_OK) {
        return false;
    }
    if (mfs_write(&file, text, sizeof(text) - 1u) != (int)(sizeof(text) - 1u)) {
        (void)mfs_close(&file);
        return false;
    }
    if (mfs_fsync(&file) != MFS_OK) {
        (void)mfs_close(&file);
        return false;
    }
    if (mfs_close(&file) != MFS_OK) {
        return false;
    }
    if (mfs_open("/alive.tmp", MFS_O_RDONLY, &file) != MFS_OK) {
        return false;
    }
    memset(back, 0, sizeof(back));
    if (mfs_read(&file, back, sizeof(back) - 1u) != (int)(sizeof(back) - 1u) ||
        memcmp(back, text, sizeof(back) - 1u) != 0) {
        (void)mfs_close(&file);
        return false;
    }
    if (mfs_close(&file) != MFS_OK || mfs_unlink("/alive.tmp") != MFS_OK || mfs_sync() != MFS_OK) {
        return false;
    }

    return mfs_stat("/alive.tmp", &inode, NULL) == MFS_ENOENT;
}

static void report_failure(struct sweep *sweep, const struct scenario *scenario, const char *mode,
                           k_u32_t first, k_u32_t second, const char *reason)
{
    sweep->reported++;
    if (sweep->reported <= 5u) {
        ktest_fail(__FILE__, __LINE__, "%s %s at %u/%u: %s", scenario->name, mode, (unsigned)first,
                   (unsigned)second, reason);
    }
}

static bool run_point(const struct scenario *scenario, struct sweep *sweep,
                      const struct k_blockdev_fault_plan *plan, const char *mode, k_u32_t first,
                      k_u32_t second)
{
    struct scenario_context ctx;
    struct mfs_check_report report;
    char reason[160];
    char operation[96];
    k_u32_t kind;
    k_u32_t sector;
    bool ok = true;

    sweep->points++;
    g_points++;
    restore_base();
    if (k_blockdev_fault_arm(&g_fault, plan) != 0) {
        report_failure(sweep, scenario, mode, first, second, "could not arm the bus");
        return false;
    }

    (void)run_scenario(scenario, &ctx);
    kind = k_blockdev_fault_kind_at(&g_fault, first);
    sector = k_blockdev_fault_sector_at(&g_fault, first);
    snprintf(operation, sizeof(operation), "%s kind=%u sector=%u", mode, (unsigned)kind,
             (unsigned)sector);
    k_blockdev_fault_reboot(&g_fault);

    if (mfs_mount(&g_fault.dev) != MFS_OK) {
        report_failure(sweep, scenario, operation, first, second, "recovery mount failed");
        mfs_unmount();
        return false;
    }

    memset(&report, 0, sizeof(report));
    if (mfs_check(&report) != MFS_OK || report.errors != 0u) {
        snprintf(reason, sizeof(reason), "fsck found %u problem(s): %s", (unsigned)report.errors,
                 report.first[0] != '\0' ? report.first : "(none)");
    } else if (!keeper_whole()) {
        snprintf(reason, sizeof(reason), "the keeper was damaged");
    } else if (!scenario->verify(&ctx)) {
        snprintf(reason, sizeof(reason), "the object is neither old, whole nor absent");
    } else if (!recovery_canary()) {
        snprintf(reason, sizeof(reason), "the recovered filesystem could not keep a canary");
    } else {
        reason[0] = '\0';
    }

    ok = reason[0] == '\0';
    mfs_unmount();

    if (ok) {
        sweep->passed++;
        g_passed++;
    } else {
        report_failure(sweep, scenario, operation, first, second, reason);
    }

    return ok;
}

static bool pair_exists(const struct reorder_pair *pairs, k_size_t count, k_u32_t first,
                        k_u32_t second)
{
    k_size_t i;

    for (i = 0; i < count; i++) {
        if (pairs[i].first == first && pairs[i].second == second) {
            return true;
        }
    }

    return false;
}

static void pair_add(struct reorder_pair *pairs, k_size_t *count, k_u32_t first, k_u32_t second)
{
    if (!pair_exists(pairs, *count, first, second)) {
        pairs[*count].first = first;
        pairs[*count].second = second;
        (*count)++;
    }
}

/* Adjacent writes exercise ordinary reordering. A write and the first write
 * after the next barrier exercise the journal's ordering contract. */
static k_size_t reorder_pairs(const struct k_blockdev_fault *fault, k_u32_t operations,
                              struct reorder_pair *pairs)
{
    k_u32_t writes[K_BLOCKDEV_FAULT_TRACE_MAX];
    k_size_t write_count = 0;
    k_size_t pair_count = 0;
    k_u32_t point;

    for (point = 1; point <= operations; point++) {
        if (k_blockdev_fault_kind_at(fault, point) == K_BLOCKDEV_FAULT_OP_WRITE) {
            writes[write_count++] = point;
        }
    }

    for (point = 1; point < write_count; point++) {
        pair_add(pairs, &pair_count, writes[point - 1u], writes[point]);
    }

    for (point = 0; point < write_count; point++) {
        k_u32_t scan;
        k_u32_t after;

        for (scan = writes[point] + 1u; scan <= operations; scan++) {
            k_u32_t kind = k_blockdev_fault_kind_at(fault, scan);
            if (kind != K_BLOCKDEV_FAULT_OP_SYNC && kind != K_BLOCKDEV_FAULT_OP_FLUSH) {
                continue;
            }

            for (after = scan + 1u; after <= operations; after++) {
                if (k_blockdev_fault_kind_at(fault, after) == K_BLOCKDEV_FAULT_OP_WRITE) {
                    pair_add(pairs, &pair_count, writes[point], after);
                    break;
                }
            }
            break;
        }
    }

    return pair_count;
}

static void sweep_scenario(const struct scenario *scenario)
{
    static const int errors[] = {KERR_IO, KERR_NOSPC, KERR_TIMEOUT};
    struct k_blockdev_fault_plan plan = {0};
    struct scenario_context ctx;
    struct sweep sweep = {0};
    struct reorder_pair *pairs;
    k_u32_t writes[K_BLOCKDEV_FAULT_TRACE_MAX];
    k_size_t write_count = 0;
    k_size_t pair_count;
    k_u32_t operations;
    k_u32_t point;
    k_size_t i;

    ktest_begin(scenario->name);
    restore_base();
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK(run_scenario(scenario, &ctx));
    operations = g_fault.stats.operations;
    KT_CHECK(operations > 0u);
    KT_CHECK(!g_fault.stats.trace_overflow);
    mfs_unmount();

    for (point = 1; point <= operations; point++) {
        if (k_blockdev_fault_kind_at(&g_fault, point) == K_BLOCKDEV_FAULT_OP_WRITE) {
            writes[write_count++] = point;
        }
    }

    pairs = (struct reorder_pair *)malloc((size_t)operations * 2u * sizeof(*pairs));
    if (pairs == NULL) {
        KT_CHECK(false);
        return;
    }
    pair_count = reorder_pairs(&g_fault, operations, pairs);

    for (point = 1; point <= operations; point++) {
        for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
            plan.modes = K_BLOCKDEV_FAULT_ERROR;
            plan.at = point;
            plan.error = errors[i];
            (void)run_point(scenario, &sweep, &plan, "error", point, 0);
        }

        plan.modes = K_BLOCKDEV_FAULT_POWER_CUT;
        plan.at = point;
        plan.error = 0;
        (void)run_point(scenario, &sweep, &plan, "power-cut", point, 0);
    }

    for (i = 0; i < write_count; i++) {
        plan.modes = K_BLOCKDEV_FAULT_DROP_WRITE;
        plan.at = writes[i];
        plan.error = 0;
        (void)run_point(scenario, &sweep, &plan, "dropped-write", writes[i], 0);
    }

    for (i = 0; i < pair_count; i++) {
        plan.modes = K_BLOCKDEV_FAULT_REORDER;
        plan.at = 0;
        plan.error = 0;
        plan.first = pairs[i].first;
        plan.second = pairs[i].second;
        (void)run_point(scenario, &sweep, &plan, "reorder", pairs[i].first, pairs[i].second);
    }

    free(pairs);
    printf("    %u point(s), %u passed; %u baseline operation(s), %u write(s), %u pair(s)\n",
           (unsigned)sweep.points, (unsigned)sweep.passed, (unsigned)operations,
           (unsigned)write_count, (unsigned)pair_count);
    KT_CHECK_INT_EQ(sweep.passed, sweep.points);
}

static bool baseline_make(void)
{
    struct k_blockdev_fault_plan plan = {0};

    if (mfs_ram_init(&g_ram, FAULT_VOLUME_BLOCKS) != 0) {
        return false;
    }

    g_stable = (k_u8_t *)malloc(FAULT_VOLUME_BYTES);
    g_base = (k_u8_t *)malloc(FAULT_VOLUME_BYTES);
    if (g_stable == NULL || g_base == NULL) {
        return false;
    }

    mfs_ram_device(&g_ram, &g_medium);
    if (k_blockdev_fault_open(&g_fault, &g_medium, g_ram.data, g_stable, FAULT_VOLUME_BYTES) != 0 ||
        k_blockdev_fault_arm(&g_fault, &plan) != 0) {
        return false;
    }

    if (mfs_format_ex(&g_fault.dev, 256, MFS_FORMAT_JOURNAL) != MFS_OK ||
        mfs_mount(&g_fault.dev) != MFS_OK || !make_file("/keep.txt", FAULT_KEEP_BYTES, 11u) ||
        !make_file("/grow.txt", FAULT_KEEP_BYTES, 22u) ||
        !make_file("/src.txt", FAULT_SRC_BYTES, 33u) ||
        !make_file("/dst.txt", FAULT_DST_BYTES, 44u) ||
        !make_file("/victim.txt", FAULT_VICTIM_BYTES, 55u) || mfs_create("/sync.txt") != MFS_OK ||
        mfs_sync() != MFS_OK) {
        mfs_unmount();
        return false;
    }

    mfs_unmount();
    if (g_fault.dev.ops->sync(g_fault.dev.ctx) != 0) {
        return false;
    }
    memcpy(g_base, g_ram.data, FAULT_VOLUME_BYTES);

    return true;
}

void test_mfs_fault(void)
{
    k_size_t i;
    bool ready;

    ktest_begin("mfs device-fault sweeps");
    ready = baseline_make();
    KT_CHECK(ready);
    if (!ready) {
        k_blockdev_fault_close(&g_fault);
        free(g_base);
        free(g_stable);
        g_base = NULL;
        g_stable = NULL;
        mfs_ram_free(&g_ram);
        return;
    }

    for (i = 0; i < sizeof(g_scenarios) / sizeof(g_scenarios[0]); i++) {
        sweep_scenario(&g_scenarios[i]);
    }

    printf("    total: %u point(s), %u passed\n", (unsigned)g_points, (unsigned)g_passed);
    KT_CHECK_INT_EQ(g_passed, g_points);

    k_blockdev_fault_close(&g_fault);
    free(g_base);
    free(g_stable);
    g_base = NULL;
    g_stable = NULL;
    mfs_ram_free(&g_ram);
}
