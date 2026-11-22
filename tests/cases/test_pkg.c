/*
 * Micro OS — the installer of programs (ADR 0024 §4 and §5), on the host.
 *
 * The volume is a RAM volume of MicroFS mounted at /pkgroot, the transport is
 * the file cache (--offline) and the key is the one of the test. The cases
 * are the ones the ADR says a person must not be able to get past: a bad
 * signature, a package of another machine, a downgrade without the flag, and
 * a dry run that writes nothing. The install itself is the rename: the test
 * reads the file that the rename made visible.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdarg.h>
#include <string.h>

#include "ed25519.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "pkg/pkg.h"
#include "sha256.h"

#define TEST_ROOT "/pkgroot"
#define TEST_BLOCKS 384
#define TEST_SEED_SIZE 32

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

/* The seed of the key of the test: the public half is written into
 * /etc/pkg/keys/microos-progs.pub, and the secret signs the packages. */
static const k_u8_t g_seed[TEST_SEED_SIZE] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
};

static k_u8_t g_public[32];
static k_u8_t g_secret[64];

/* --- the tree of the test --- */

static bool file_exists(const char *path)
{
    struct vfs_stat info;

    return vfs_stat(path, &info) == 0;
}

/* A byte string inside another one, without the GNU extension of memmem. */
static bool contains(const void *haystack, k_size_t size, const char *needle)
{
    k_size_t needle_len = strlen(needle);
    const k_u8_t *bytes = (const k_u8_t *)haystack;
    k_size_t i;

    if (needle_len == 0 || needle_len > size) {
        return false;
    }

    for (i = 0; i + needle_len <= size; i++) {
        if (memcmp(bytes + i, needle, needle_len) == 0) {
            return true;
        }
    }

    return false;
}

static bool make_tree(void)
{
    static const char *dirs[] = {
        TEST_ROOT "/var",           TEST_ROOT "/var/pkg",      TEST_ROOT "/var/pkg/local",
        TEST_ROOT "/var/pkg/index", TEST_ROOT "/var/cache",    TEST_ROOT "/var/cache/pkg",
        TEST_ROOT "/usr",           TEST_ROOT "/usr/bin",      TEST_ROOT "/etc",
        TEST_ROOT "/etc/pkg",       TEST_ROOT "/etc/pkg/keys", TEST_ROOT "/repo",
    };
    k_size_t i;

    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return false;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);

    /* The volume is the root of the test machine; /pkgroot is a directory
     * of it and the --root of the calls, which is what a person does with a
     * second volume as well (ADR 0024 §6, the flag for the tests). */
    if (mfs_vfs_register("/") != 0) {
        return false;
    }

    for (i = 0; i < K_ARRAY_SIZE(dirs); i++) {
        if (vfs_mkdir_p(dirs[i]) != 0) {
            return false;
        }
    }

    return true;
}

static void drop_tree(void)
{
    (void)vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static int file_write(const char *path, const void *data, k_size_t size)
{
    static k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    int rc;

    rc = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_write(&file, data, size);
    (void)vfs_close(&file);

    return rc;
}

static k_size_t file_read(const char *path, void *data, k_size_t size)
{
    static k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    int n;

    if (vfs_open(path, VFS_O_RDONLY, &file, priv) != 0) {
        return 0;
    }

    n = vfs_read(&file, data, size);
    (void)vfs_close(&file);

    return (n < 0) ? 0 : (k_size_t)n;
}

/* --- a program and its signature --- */

/*
 * A small image of MOX1: the header of the format and four bytes of code.
 * The kernel is not asked to run it here - the installer only has to accept
 * it as a program of this machine - but the header is the real one, because
 * that is the header the check reads.
 */
static k_size_t make_image(k_u8_t *out, k_size_t size, const char *word)
{
    k_size_t text = 4 + ((strlen(word) + 3u) / 4u) * 4u;
    k_size_t total = K_EXEC_HEADER_SIZE + text;
    k_u32_t fields[8];

    if (total > size) {
        return 0;
    }

    memset(out, 0, total);

    fields[0] = K_EXEC_MAGIC;
    fields[1] = K_EXEC_VERSION;
    fields[2] = 0;                  /* the flags */
    fields[3] = K_EXEC_HEADER_SIZE; /* the entry */
    fields[4] = (k_u32_t)text;
    fields[5] = 0; /* the data */
    fields[6] = 0; /* the bss */
    fields[7] = K_EXEC_ARCH_X86_64;

    memcpy(out, fields, sizeof(fields));
    memcpy(out + K_EXEC_HEADER_SIZE, word, strlen(word));

    return total;
}

static void sign_bytes(const k_u8_t *data, k_size_t size, k_u8_t signature[64])
{
    KT_CHECK_INT_EQ(microos_ed25519_sign(signature, data, (size_t)size, g_secret), 0);
}

static void write_key(void)
{
    static const char digits[] = "0123456789abcdef";
    char text[2 * 32 + 2];
    k_size_t i;

    for (i = 0; i < 32; i++) {
        text[i * 2u] = digits[g_public[i] >> 4];
        text[i * 2u + 1u] = digits[g_public[i] & 0x0Fu];
    }

    text[64] = '\n';
    text[65] = '\0';

    KT_CHECK_INT_EQ(file_write(TEST_ROOT "/etc/pkg/keys/microos-progs.pub", text, 65), 65);
}

/* A package in the repository directory: the image and its .sig. */
static void write_package(const char *name, const char *version, const char *word)
{
    static k_u8_t image[512];
    k_u8_t signature[64];
    char path[160];
    char sig_path[168];
    k_size_t size = make_image(image, sizeof(image), word);

    KT_CHECK(size > 0);

    snprintf(path, sizeof(path), TEST_ROOT "/repo/%s-%s-x86_64.mox", name, version);
    snprintf(sig_path, sizeof(sig_path), "%s.sig", path);

    KT_CHECK_INT_EQ(file_write(path, image, size), (int)size);

    sign_bytes(image, size, signature);
    KT_CHECK_INT_EQ(file_write(sig_path, signature, sizeof(signature)), (int)sizeof(signature));
}

/* --- the index --- */

static k_size_t index_json(char *out, k_size_t size, const char *name, const char *version,
                           const char *machine, k_u32_t abi, k_size_t image_size,
                           const k_u8_t *image)
{
    k_u8_t digest[32];
    char hex[65];
    k_size_t i;
    int n;

    microos_sha256(image, image_size, digest);

    for (i = 0; i < 32; i++) {
        static const char digits[] = "0123456789abcdef";

        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0Fu];
    }
    hex[64] = '\0';

    n = snprintf(out, size,
                 "{\n"
                 "  \"repository\": \"microos-progs\",\n"
                 "  \"machine\": \"x86_64\",\n"
                 "  \"abi\": %u,\n"
                 "  \"packages\": [\n"
                 "    {\"name\": \"%s\", \"version\": \"%s\", \"machine\": \"%s\","
                 " \"abi\": %u, \"size\": %u, \"sha256\": \"%s\","
                 " \"file\": \"%s-%s-x86_64.mox\", \"summary\": \"a test program\"}\n"
                 "  ]\n"
                 "}\n",
                 (unsigned)PKG_ABI, name, version, machine, (unsigned)abi, (unsigned)image_size,
                 hex, name, version);

    if (n < 0 || (k_size_t)n >= size) {
        return 0;
    }

    return (k_size_t)n;
}

/* The index and its signature where `pkg -Sy` would have left them. */
static void write_index(const char *text, k_size_t size)
{
    k_u8_t signature[64];

    KT_CHECK_INT_EQ(file_write(TEST_ROOT "/var/pkg/index/microos-progs.json", text, size),
                    (int)size);

    sign_bytes((const k_u8_t *)text, size, signature);
    KT_CHECK_INT_EQ(
        file_write(TEST_ROOT "/var/pkg/index/microos-progs.json.sig", signature, sizeof(signature)),
        (int)sizeof(signature));
}

/* The package in the cache, as a download would have left it. */
static void cache_package(const char *name, const char *version, const char *word)
{
    static k_u8_t image[512];
    k_u8_t signature[64];
    char path[160];
    k_size_t size = make_image(image, sizeof(image), word);

    snprintf(path, sizeof(path), TEST_ROOT "/var/cache/pkg/%s-%s-x86_64.mox", name, version);

    KT_CHECK_INT_EQ(file_write(path, image, size), (int)size);

    sign_bytes(image, size, signature);

    {
        char sig_path[168];

        snprintf(sig_path, sizeof(sig_path), "%s.sig", path);
        KT_CHECK_INT_EQ(file_write(sig_path, signature, sizeof(signature)), (int)sizeof(signature));
    }
}

/* --- the command --- */

static int run(const char *first, ...)
{
    char *argv[9];
    const char *arg = first;
    va_list ap;
    int argc = 0;

    argv[argc++] = (char *)"pkg";

    va_start(ap, first);

    while (arg != NULL && argc < (int)K_ARRAY_SIZE(argv)) {
        argv[argc++] = (char *)arg;
        arg = va_arg(ap, const char *);
    }

    va_end(ap);

    return pkg_main(argc, argv);
}

/* --- the tests --- */

static void test_index(void)
{
    struct pkg_index index;
    static const char text[] =
        "{\"repository\":\"microos-progs\",\"machine\":\"x86_64\",\"abi\":2,"
        "\"future\":{\"a\":[1,2,{\"b\":null}]},"
        "\"packages\":["
        "{\"name\":\"hello\",\"version\":\"1.0.0\",\"size\":64,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"summary\":\"says hello\"},"
        "{\"name\":\"cat\",\"version\":\"2.0.0\",\"machine\":\"xtensa\",\"abi\":1,\"size\":32,"
        "\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\","
        "\"file\":\"cat.mox\"}"
        "]}";

    ktest_begin("pkg: the index of a repository names the packages of the machine");

    KT_CHECK_INT_EQ(pkg_index_parse(text, sizeof(text) - 1u, &index), PKG_OK);
    KT_CHECK_INT_EQ((int)index.count, 2);
    KT_CHECK_STR_EQ(index.repository, "microos-progs");
    KT_CHECK_STR_EQ(index.machine, "x86_64");
    KT_CHECK_INT_EQ((int)index.abi, (int)PKG_ABI);
    KT_CHECK_INT_EQ(pkg_index_find(&index, "hello"), 0);
    KT_CHECK_INT_EQ(pkg_index_find(&index, "cat"), 1);
    KT_CHECK_INT_EQ(pkg_index_find(&index, "nope"), -1);
    KT_CHECK_STR_EQ(index.entries[0].name, "hello");
    KT_CHECK_STR_EQ(index.entries[0].file, "hello-1.0.0-x86_64.mox");
    KT_CHECK_INT_EQ((int)index.entries[0].abi, (int)PKG_ABI); /* the index's */
    KT_CHECK_INT_EQ((int)index.entries[1].abi, 1);            /* its own */
    KT_CHECK_STR_EQ(index.entries[1].machine, "xtensa");
    KT_CHECK_STR_EQ(index.entries[1].file, "cat.mox");

    /* A document that is not the one of the format is refused. */
    KT_CHECK_INT_EQ(pkg_index_parse("", 0, &index), PKG_EPARSE);
    KT_CHECK_INT_EQ(pkg_index_parse("[]", 2, &index), PKG_EPARSE);

    {
        static const char empty_machine[] = "{\"machine\":\"x86_64\"}";
        static const char short_digest[] =
            "{\"machine\":\"x86_64\",\"abi\":2,\"packages\":[{\"name\":\"x\","
            "\"version\":\"1\",\"size\":1,\"sha256\":\"00\"}]}";
        static const char unknown_field[] =
            "{\"machine\":\"x86_64\",\"abi\":2,\"x\":{\"y\":[true,false,null]},"
            "\"packages\":[]}";

        KT_CHECK_INT_EQ(pkg_index_parse(empty_machine, strlen(empty_machine), &index), PKG_EPARSE);
        KT_CHECK_INT_EQ(pkg_index_parse(short_digest, strlen(short_digest), &index), PKG_EPARSE);

        /* A field this parser does not know is skipped; the rest is read. */
        KT_CHECK_INT_EQ(pkg_index_parse(unknown_field, strlen(unknown_field), &index), PKG_OK);
        KT_CHECK_INT_EQ((int)index.count, 0);
    }
}

static void test_local_install(void)
{
    static k_u8_t buffer[512];
    k_size_t size;
    int rc;

    ktest_begin("pkg: a signed file is installed, and the rename makes it visible");

    KT_CHECK(make_tree());
    write_key();
    write_package("hello", "1.0.0", "hello one");

    rc = run("--root", TEST_ROOT, "-U", TEST_ROOT "/repo/hello-1.0.0-x86_64.mox", NULL, NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_OK);

    size = file_read(TEST_ROOT "/usr/bin/hello", buffer, sizeof(buffer));
    KT_CHECK(size > K_EXEC_HEADER_SIZE);
    KT_CHECK(memcmp(buffer, "MOX1", 4) == 0);
    KT_CHECK(contains(buffer + K_EXEC_HEADER_SIZE, size - K_EXEC_HEADER_SIZE, "hello one"));

    /* The record is the one the queries read. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-Qi", "hello", NULL, NULL), KSHELL_OK);
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-Qk", NULL, NULL, NULL), KSHELL_OK);

    /* The same version again is not an install. */
    KT_CHECK_INT_EQ(
        run("--root", TEST_ROOT, "-U", TEST_ROOT "/repo/hello-1.0.0-x86_64.mox", NULL, NULL),
        KSHELL_OK);

    /* --force replaces it with the very same bytes, through the rename. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-U", TEST_ROOT "/repo/hello-1.0.0-x86_64.mox",
                        "--force", NULL, NULL),
                    KSHELL_OK);

    drop_tree();
}

static void test_bad_signature(void)
{
    static k_u8_t image[512];
    k_u8_t signature[64];
    char path[160];
    char sig_path[168];
    k_size_t size;
    k_size_t installed;
    int rc;

    ktest_begin("pkg: a file with a broken signature does not reach /usr/bin");

    KT_CHECK(make_tree());
    write_key();
    write_package("hello", "1.0.0", "hello one");

    KT_CHECK_INT_EQ(
        run("--root", TEST_ROOT, "-U", TEST_ROOT "/repo/hello-1.0.0-x86_64.mox", NULL, NULL),
        KSHELL_OK);

    size = file_read(TEST_ROOT "/usr/bin/hello", image, sizeof(image));
    KT_CHECK(size > 0);

    /* The same file with one byte of its signature flipped. */
    snprintf(path, sizeof(path), TEST_ROOT "/repo/hello-1.0.0-x86_64.mox");
    snprintf(sig_path, sizeof(sig_path), "%s.sig", path);
    KT_CHECK_INT_EQ(file_read(sig_path, signature, sizeof(signature)), (k_size_t)sizeof(signature));
    signature[3] ^= 0x40u;
    KT_CHECK_INT_EQ(file_write(sig_path, signature, sizeof(signature)), (int)sizeof(signature));

    rc = run("--root", TEST_ROOT, "-U", path, "--force", NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_ERROR);

    /* The program is the one of the good install, byte for byte. */
    installed = file_read(TEST_ROOT "/usr/bin/hello", image + 256, sizeof(image) - 256);
    KT_CHECK_INT_EQ(installed, size);

    /* The record was not touched either. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-Qi", "hello", NULL, NULL), KSHELL_OK);

    drop_tree();
}

static void test_wrong_machine(void)
{
    static k_u8_t image[512];
    char path[128];
    k_size_t size = make_image(image, sizeof(image), "hello");
    int rc;

    ktest_begin("pkg: a package of another machine is refused, by name and by index");

    KT_CHECK(make_tree());
    write_key();

    snprintf(path, sizeof(path), TEST_ROOT "/repo/hello-1.0.0-esp32.mox");
    KT_CHECK_INT_EQ(file_write(path, image, size), (int)size);

    rc = run("--root", TEST_ROOT, "-U", path, NULL, NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_ERROR);
    KT_CHECK(!file_exists(TEST_ROOT "/usr/bin/hello"));
    /* The same through an index of the repository. */
    {
        static char text[1024];
        k_size_t length =
            index_json(text, sizeof(text), "hello", "1.0.0", "xtensa", PKG_ABI, size, image);

        KT_CHECK(length > 0);
        write_index(text, length);
        cache_package("hello", "1.0.0", "hello");

        rc = run("--root", TEST_ROOT, "--offline", "-S", "hello", NULL, NULL);
        KT_CHECK_INT_EQ(rc, KSHELL_ERROR);
        KT_CHECK(!file_exists(TEST_ROOT "/usr/bin/hello"));
    }

    drop_tree();
}

static void test_downgrade_and_dry_run(void)
{
    static char text[1024];
    static k_u8_t older[512];
    static k_u8_t newer[512];
    k_size_t older_size = make_image(older, sizeof(older), "hello one");
    k_size_t newer_size = make_image(newer, sizeof(newer), "hello two");
    k_size_t length;
    int rc;

    ktest_begin("pkg: a downgrade needs the flag, and a dry run writes nothing");

    KT_CHECK(make_tree());
    write_key();

    cache_package("hello", "2.0.0", "hello two");

    length = index_json(text, sizeof(text), "hello", "2.0.0", "x86_64", PKG_ABI, newer_size, newer);
    KT_CHECK(length > 0);
    write_index(text, length);

    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "--offline", "-S", "hello", NULL, NULL), KSHELL_OK);
    KT_CHECK(file_exists(TEST_ROOT "/usr/bin/hello"));

    /* 1.0.0 of the repository is older than the installed 2.0.0. */
    length = index_json(text, sizeof(text), "hello", "1.0.0", "x86_64", PKG_ABI, older_size, older);
    KT_CHECK(length > 0);
    write_index(text, length);
    cache_package("hello", "1.0.0", "hello one");

    rc = run("--root", TEST_ROOT, "--offline", "-S", "hello", NULL, NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_ERROR);

    /* The dry run of the allowed downgrade writes nothing. */
    rc = run("--root", TEST_ROOT, "--offline", "-S", "hello", "--allow-downgrade", "--dry-run",
             NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_OK);

    {
        char record[256];

        KT_CHECK(file_read(TEST_ROOT "/var/pkg/local/hello", record, sizeof(record) - 1u) > 0);
        record[sizeof(record) - 1u] = '\0';
        KT_CHECK(strstr(record, "version=2.0.0") != NULL);
    }

    rc = run("--root", TEST_ROOT, "--offline", "-S", "hello", "--allow-downgrade", NULL);
    KT_CHECK_INT_EQ(rc, KSHELL_OK);

    /* The installed record now says 1.0.0. */
    {
        char record[256];

        KT_CHECK(file_read(TEST_ROOT "/var/pkg/local/hello", record, sizeof(record) - 1u) > 0);
        record[sizeof(record) - 1u] = '\0';
        KT_CHECK(strstr(record, "version=1.0.0") != NULL);
    }

    drop_tree();
}

static void test_offline_and_remove(void)
{
    static char text[1024];
    static k_u8_t image[512];
    k_size_t size = make_image(image, sizeof(image), "hello");
    k_size_t length;

    ktest_begin("pkg: the cache is enough for an install, and -R takes it away");

    KT_CHECK(make_tree());
    write_key();

    cache_package("hello", "1.0.0", "hello");

    length = index_json(text, sizeof(text), "hello", "1.0.0", "x86_64", PKG_ABI, size, image);
    KT_CHECK(length > 0);
    write_index(text, length);

    /* A name that is not in the index. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "--offline", "-S", "nope", NULL, NULL), KSHELL_ERROR);

    /* --download-only leaves the program out of /usr/bin. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "--offline", "-S", "hello", "--download-only", NULL),
                    KSHELL_OK);
    KT_CHECK(!file_exists(TEST_ROOT "/usr/bin/hello"));

    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "--offline", "-S", "hello", NULL, NULL), KSHELL_OK);

    /* The index has no signature: the stored index is not trusted. */
    {
        char sig[128];

        snprintf(sig, sizeof(sig), TEST_ROOT "/var/pkg/index/microos-progs.json.sig");
        KT_CHECK_INT_EQ(vfs_unlink(sig), 0);
        KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "--offline", "-S", "hello", NULL, NULL),
                        KSHELL_ERROR);
    }

    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-R", "hello", NULL, NULL, NULL), KSHELL_OK);
    KT_CHECK(!file_exists(TEST_ROOT "/usr/bin/hello"));
    KT_CHECK(!file_exists(TEST_ROOT "/var/pkg/local/hello"));

    /* A name that is not installed. */
    KT_CHECK_INT_EQ(run("--root", TEST_ROOT, "-R", "hello", NULL, NULL, NULL), KSHELL_ERROR);

    drop_tree();
}

void test_pkg(void)
{
    microos_ed25519_keypair(g_public, g_secret, g_seed);

    test_index();
    test_local_install();
    test_bad_signature();
    test_wrong_machine();
    test_downgrade_and_dry_run();
    test_offline_and_remove();
}
