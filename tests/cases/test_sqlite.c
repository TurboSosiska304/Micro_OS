/*
 * Micro OS — the tests of SQLite, the database library of the public domain.
 *
 * The library is the upstream amalgamation of `third_party/sqlite`, compiled
 * for the host with SQLITE_OS_OTHER=1 and the VFS of the port
 * (`port/sqlite_vfs.c`) with SQLITE_PORT_HOST. The host is the second build
 * of the same source: the calls under the VFS are the libc of the host, the
 * shape of the errors is the one the VFS gives SQLite, and the file is a file
 * of the host.
 *
 * The test covers the three shapes of a database - a created file, a table
 * with a whole number, a text, a real and a block, a transaction that rolls
 * back, and the journal modes that are files here (DELETE and PERSIST) - and
 * the second machine is `make x86-smoke` and `make x86-disk-smoke`, which run
 * /bin/sqlite3 over the same bytes of MicroFS.
 *
 * The guest program asks for the journal of a file as well: a task of that
 * machine has sixteen descriptors, and the smoke test
 * (tools/x86_fs_smoke.py) interrupts a transaction with a reset and reads
 * the rows that the journal of the next boot rolled back.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define SQLITE_TEST_PATH "test_sqlite.db"

static int exec_sql(sqlite3 *db, const char *sql)
{
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s: %s\n", sql, (message != NULL) ? message : "no message");
        sqlite3_free(message);
    }

    return rc;
}

static int scalar_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *statement = NULL;
    int value = -1;

    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        return -1;
    }

    if (sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int(statement, 0);
    }

    sqlite3_finalize(statement);
    return value;
}

/* The journal modes a file of this system can carry. MEMORY is the one the
 * program of the guest asks for (four descriptors); DELETE and PERSIST are
 * the two the VFS implements and this test exercises. */
static void test_journal_modes(sqlite3 *db)
{
    sqlite3_stmt *statement = NULL;

    KT_CHECK(exec_sql(db, "PRAGMA journal_mode=DELETE") == SQLITE_OK);
    KT_CHECK(sqlite3_prepare_v2(db, "PRAGMA journal_mode=DELETE", -1, &statement, NULL) ==
             SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK_STR_EQ((const char *)sqlite3_column_text(statement, 0), "delete");
    sqlite3_finalize(statement);

    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "PRAGMA journal_mode=PERSIST", -1, &statement, NULL) ==
             SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK_STR_EQ((const char *)sqlite3_column_text(statement, 0), "persist");
    sqlite3_finalize(statement);

    /* A write in the mode, and the next statement still sees it. */
    KT_CHECK(exec_sql(db, "INSERT INTO items(name, price) VALUES('persist', 4.5)") == SQLITE_OK);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items WHERE name='persist'"), 1);
    KT_CHECK(exec_sql(db, "DELETE FROM items WHERE name='persist'") == SQLITE_OK);

    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "PRAGMA journal_mode=DELETE", -1, &statement, NULL) ==
             SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    sqlite3_finalize(statement);
}

static void test_open_and_shape(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    struct stat info;
    int rc;

    ktest_begin("sqlite: the VFS of the port is the default and opens a file");

    (void)unlink(SQLITE_TEST_PATH);
    (void)unlink(SQLITE_TEST_PATH "-journal");

    rc = sqlite3_open(SQLITE_TEST_PATH, &db);
    KT_CHECK_INT_EQ(rc, SQLITE_OK);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    /* The name of the VFS is the one of the port, not "unix": a file of this
     * machine is opened by port/sqlite_vfs.c and not by the layer of a POSIX
     * kernel that the library was built without. */
    KT_CHECK(sqlite3_vfs_find("microos") != NULL);
    KT_CHECK_STR_EQ(sqlite3_vfs_find(NULL)->zName, "microos");

    KT_CHECK(exec_sql(db, "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT,"
                          " price REAL, data BLOB)") == SQLITE_OK);

    /* The three types of a column and the binding of an argument. */
    KT_CHECK(sqlite3_prepare_v2(db, "INSERT INTO items(name, price, data) VALUES(?, ?, ?)", -1,
                                &statement, NULL) == SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_bind_text(statement, 1, "one", -1, SQLITE_STATIC), SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_bind_double(statement, 2, 1.5), SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_bind_blob(statement, 3, "\x01\x02", 2, SQLITE_STATIC), SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);

    KT_CHECK(exec_sql(db, "INSERT INTO items(name, price, data) VALUES('two', 2.25, x'03')") ==
             SQLITE_OK);

    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items"), 2);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT id FROM items WHERE name='two'"), 2);

    /* The real number is a double and not a text: the guest computes it with
     * the SSE registers the port turns on for this program. */
    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "SELECT price FROM items WHERE id=1", -1, &statement, NULL) ==
             SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK_INT_EQ(sqlite3_column_type(statement, 0), SQLITE_FLOAT);
    KT_CHECK(sqlite3_column_double(statement, 0) == 1.5);
    sqlite3_finalize(statement);

    /* The block comes back byte for byte. */
    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "SELECT data FROM items WHERE id=1", -1, &statement, NULL) ==
             SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK_INT_EQ(sqlite3_column_type(statement, 0), SQLITE_BLOB);
    KT_CHECK_INT_EQ(sqlite3_column_bytes(statement, 0), 2);
    KT_CHECK(memcmp(sqlite3_column_blob(statement, 0), "\x01\x02", 2) == 0);
    sqlite3_finalize(statement);

    /* The update and the delete of a row. */
    KT_CHECK(exec_sql(db, "UPDATE items SET price = 3.5 WHERE name = 'two'") == SQLITE_OK);
    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "SELECT price FROM items WHERE name='two'", -1, &statement,
                                NULL) == SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK(sqlite3_column_double(statement, 0) == 3.5);
    sqlite3_finalize(statement);

    KT_CHECK(exec_sql(db, "DELETE FROM items WHERE name = 'two'") == SQLITE_OK);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items"), 1);

    /* The transaction that rolls back: the inserted row is not there and the
     * file has the size it had before the transaction. The rollback of a
     * database that grew asks the VFS for a truncation. */
    KT_CHECK(exec_sql(db, "BEGIN") == SQLITE_OK);
    KT_CHECK(exec_sql(db, "INSERT INTO items(name, price) VALUES('roll', 9.0)") == SQLITE_OK);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items"), 2);
    KT_CHECK(exec_sql(db, "ROLLBACK") == SQLITE_OK);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items"), 1);

    test_journal_modes(db);

    /* The check of the library itself: `ok` is the only answer that means
     * the pages agree with the schema and the indexes. */
    statement = NULL;
    KT_CHECK(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL) == SQLITE_OK);
    KT_CHECK_INT_EQ(sqlite3_step(statement), SQLITE_ROW);
    KT_CHECK_STR_EQ((const char *)sqlite3_column_text(statement, 0), "ok");
    sqlite3_finalize(statement);

    KT_CHECK_INT_EQ(sqlite3_close(db), SQLITE_OK);

    KT_CHECK(stat(SQLITE_TEST_PATH, &info) == 0);
    KT_CHECK(info.st_size > 0);
}

static void test_reopen(void)
{
    sqlite3 *db = NULL;
    int rc;

    ktest_begin("sqlite: the file of the port survives a close and comes back");

    rc = sqlite3_open(SQLITE_TEST_PATH, &db);
    KT_CHECK_INT_EQ(rc, SQLITE_OK);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items"), 1);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM items WHERE name='one'"), 1);

    KT_CHECK(exec_sql(db, "PRAGMA integrity_check") == SQLITE_OK);
    KT_CHECK_INT_EQ(scalar_int(db, "SELECT count(*) FROM sqlite_schema"), 1);

    KT_CHECK_INT_EQ(sqlite3_close(db), SQLITE_OK);
    KT_CHECK(unlink(SQLITE_TEST_PATH) == 0);
}

void test_sqlite(void)
{
    test_open_and_shape();
    test_reopen();
}
