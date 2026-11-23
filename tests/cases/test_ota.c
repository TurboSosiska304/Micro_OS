/*
 * Micro OS — the update over the network, on the host (ADR 0022 §7).
 *
 * The transport is a fake server: it answers every request with one signed
 * image from memory, which is the "wire" without a network. The slots and
 * the record of the state are files of a RAM volume of MicroFS, and a
 * "reboot" is the call of ota_boot() a second time - the same state machine
 * the board's bootloader keeps in otadata, written where a test can read it.
 *
 * What the cases prove, in the order of the promise:
 *
 *   - an image whose signature does not check never reaches a slot, and the
 *     slot it would have taken still holds the bytes it had;
 *   - an install writes the inactive slot and leaves it PENDING;
 *   - a boot that confirms the slot keeps it, and one that does not returns
 *     the machine to the other slot;
 *   - the length of the slot is a ceiling in the first place, before the
 *     signature is even asked.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "ed25519.h"
#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "net/ota.h"
#include "sha256.h"
#include "sign_image.h"

#define TEST_BLOCKS 2048
#define TEST_SEED_SIZE 32
#define TEST_PAYLOAD 3000u

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static const k_u8_t g_seed[TEST_SEED_SIZE] = {
    0x4a, 0x8f, 0x2c, 0x11, 0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4,
    0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03,
};

static const k_u8_t g_other_seed[TEST_SEED_SIZE] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

static k_u8_t g_public[32];
static k_u8_t g_secret[64];
static k_u8_t g_other_public[32];
static k_u8_t g_other_secret[64];
static k_u8_t g_image[OTA_IMAGE_MAX + 128];
static k_u8_t g_payload[TEST_PAYLOAD];

/* --- the wire: a server that answers one image --- */

struct fake_server {
    const k_u8_t *body;
    k_size_t size;
    k_size_t sent;
    bool answered;
    int requests;
};

static int fake_open(const struct http_transport *self, const char *host, int port, void **ctx)
{
    struct fake_server *server = (struct fake_server *)self->user;

    (void)host;
    (void)port;

    if (server == NULL) {
        return -1;
    }

    *ctx = server;

    return 0;
}

static int fake_send(void *ctx, const void *data, k_size_t size)
{
    struct fake_server *server = (struct fake_server *)ctx;

    (void)data;

    server->requests++;

    return (int)size;
}

/* The head first, then the body in blocks: the client reads the same shape
 * it would read from a socket. */
static int fake_recv(void *ctx, void *data, k_size_t size)
{
    struct fake_server *server = (struct fake_server *)ctx;
    char head[128];
    k_size_t head_len;

    if (!server->answered) {
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                         "Content-Length: %u\r\nConnection: close\r\n\r\n",
                         (unsigned)server->size);

        if (n <= 0 || (k_size_t)n >= sizeof(head)) {
            return -1;
        }

        head_len = (k_size_t)n;

        if (size < head_len) {
            return -1;
        }

        memcpy(data, head, head_len);
        server->answered = true;

        return (int)head_len;
    }

    if (server->sent >= server->size) {
        return 0;
    }

    {
        k_size_t take = server->size - server->sent;

        if (take > size) {
            take = size;
        }

        memcpy(data, server->body + server->sent, take);
        server->sent += take;

        return (int)take;
    }
}

static void fake_close(void *ctx)
{
    (void)ctx;
}

static struct http_transport g_fake = {
    .open = fake_open,
    .send = fake_send,
    .recv = fake_recv,
    .close = fake_close,
    .user = NULL,
};

static struct fake_server g_server;

/* --- the volume and the image --- */

static bool make_tree(void)
{
    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return false;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);

    if (mfs_vfs_register("/") != 0) {
        return false;
    }

    if (vfs_mkdir_p("/var") != 0 || vfs_mkdir_p("/etc/ota") != 0) {
        return false;
    }

    microos_ed25519_keypair(g_public, g_secret, g_seed);
    microos_ed25519_keypair(g_other_public, g_other_secret, g_other_seed);

    {
        static const char digits[] = "0123456789abcdef";
        char text[OTA_KEY_HEX + 1];
        k_u8_t priv[VFS_FILE_DATA_SIZE];
        struct vfs_file file;
        k_size_t i;

        for (i = 0; i < 32; i++) {
            text[i * 2u] = digits[g_public[i] >> 4];
            text[i * 2u + 1u] = digits[g_public[i] & 0x0Fu];
        }
        text[64] = '\n';
        text[65] = '\0';

        if (vfs_open("/etc/ota/image.pub", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) !=
            0) {
            return false;
        }
        (void)vfs_write(&file, text, 65);
        (void)vfs_close(&file);
    }

    for (k_size_t i = 0; i < sizeof(g_payload); i++) {
        g_payload[i] = (k_u8_t)((i * 13u + 5u) & 0xFFu);
    }

    return true;
}

static void drop_tree(void)
{
    (void)vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* A payload and its trailer, signed with `secret` (or not signed at all). */
static k_size_t make_image(const k_u8_t *payload, k_size_t size, const k_u8_t *secret)
{
    k_u8_t digest[MICROOS_SHA256_SIZE];
    k_u8_t *trailer = g_image + size;

    memcpy(g_image, payload, size);

    microos_sha256(payload, size, digest);
    microos_sign_trailer_make(trailer, (uint64_t)size, digest);

    if (secret != NULL) {
        if (microos_ed25519_sign(trailer + MICROOS_SIGN_HEADER_SIZE, trailer,
                                 MICROOS_SIGN_HEADER_SIZE, secret) != 0) {
            return 0;
        }
    }

    return size + MICROOS_SIGN_TRAILER_SIZE;
}

static void serve(const k_u8_t *body, k_size_t size)
{
    memset(&g_server, 0, sizeof(g_server));
    g_server.body = body;
    g_server.size = size;
    g_fake.user = &g_server;
}

static int install(const char *url, struct ota_state *out)
{
    struct ota_config config;

    memset(&config, 0, sizeof(config));
    config.url = url;
    config.state = OTA_STATE_DEFAULT;
    config.key_path = "/etc/ota/image.pub";
    config.transport = &g_fake;

    return ota_install(&config, out);
}

/* The bytes of a slot, and its length. */
static int inactive_slot(const struct ota_state *state)
{
    return state->active == 0 ? 1 : 0;
}

static k_size_t slot_bytes(int slot, k_u8_t *out, k_size_t size)
{
    static k_u8_t priv[VFS_FILE_DATA_SIZE];
    char path[64];
    struct vfs_file file;
    int n;

    snprintf(path, sizeof(path), OTA_STATE_DEFAULT "/slot%d.img", slot);

    if (vfs_open(path, VFS_O_RDONLY, &file, priv) != 0) {
        return 0;
    }

    n = vfs_read(&file, out, size);
    (void)vfs_close(&file);

    return n > 0 ? (k_size_t)n : 0;
}

/* --- the cases --- */

static void test_install_and_confirm(void)
{
    struct ota_state state;
    static k_u8_t bytes[TEST_PAYLOAD + 16];
    k_size_t size;
    k_size_t image;

    ktest_begin("ota: a slot, its record and no update yet");

    KT_CHECK_INT_EQ(ota_status(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.active, 0);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_VALID);
    KT_CHECK_INT_EQ((int)state.sequence, 0);

    image = make_image(g_payload, TEST_PAYLOAD, g_secret);
    KT_CHECK_INT_EQ((int)image, (int)(TEST_PAYLOAD + OTA_TRAILER_SIZE));
    serve(g_image, image);

    ktest_begin("ota: a signed image is written into the inactive slot");

    KT_CHECK_INT_EQ(install("http://test/update.img", &state), OTA_INSTALLED);
    KT_CHECK_INT_EQ(state.active, 1);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_NEW);
    KT_CHECK_INT_EQ((int)state.sequence, 1);

    /* The slot holds the payload and not the trailer: the trailer belongs to
     * the envelope of the image, and the payload is what boots. */
    size = slot_bytes(1, bytes, sizeof(bytes));
    KT_CHECK_INT_EQ((int)size, (int)TEST_PAYLOAD);
    KT_CHECK(memcmp(bytes, g_payload, TEST_PAYLOAD) == 0);

    ktest_begin("ota: the boot of the new slot, and its confirmation");

    KT_CHECK_INT_EQ(ota_boot(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.active, 1);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_PENDING);
    KT_CHECK(!state.rolled_back);

    KT_CHECK_INT_EQ(ota_confirm(), OTA_OK);
    KT_CHECK_INT_EQ(ota_status(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_VALID);

    /* The boot after the confirmation keeps the slot. */
    KT_CHECK_INT_EQ(ota_boot(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.active, 1);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_VALID);
    KT_CHECK(!state.rolled_back);
}

static void test_rollback(void)
{
    struct ota_state state;
    static k_u8_t other[TEST_PAYLOAD];
    static k_u8_t bytes[TEST_PAYLOAD + 16];
    k_size_t i;
    k_size_t image;

    for (i = 0; i < sizeof(other); i++) {
        other[i] = (k_u8_t)((i * 7u + 1u) & 0xFFu);
    }

    image = make_image(other, sizeof(other), g_secret);
    serve(g_image, image);

    ktest_begin("ota: an installed slot is pending until the boot confirms it");

    KT_CHECK_INT_EQ(install("http://test/update.img", &state), OTA_INSTALLED);
    KT_CHECK_INT_EQ(state.active, 0);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_NEW);

    /* The boot of the new slot: it is running, and it has not confirmed. */
    KT_CHECK_INT_EQ(ota_boot(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.active, 0);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_PENDING);
    KT_CHECK(!state.rolled_back);

    /* The boot after that: the slot never confirmed itself, and the machine
     * returns to the other one - the rollback of ADR 0022. */
    KT_CHECK_INT_EQ(ota_boot(&state), OTA_OK);
    KT_CHECK_INT_EQ(state.active, 1);
    KT_CHECK_INT_EQ(state.slot_state, OTA_SLOT_VALID);
    KT_CHECK(state.rolled_back);

    /* The machine runs the slot it had, and the payload there is still the
     * one it booted: a rollback does not touch it. The slot it returned from
     * keeps its bytes - the board's bootloader marks it aborted in otadata
     * and leaves the flash as it is. */
    KT_CHECK_INT_EQ((int)slot_bytes(1, bytes, sizeof(bytes)), (int)TEST_PAYLOAD);
    KT_CHECK(memcmp(bytes, g_payload, TEST_PAYLOAD) == 0);
}

static void test_refusals(void)
{
    struct ota_state before;
    struct ota_state after;
    static k_u8_t bytes[TEST_PAYLOAD + 16];
    k_size_t image;
    k_size_t size;

    KT_CHECK_INT_EQ(ota_status(&before), OTA_OK);
    size = slot_bytes(before.active, bytes, sizeof(bytes));

    ktest_begin("ota: a byte changed on the wire is refused before the slot");

    image = make_image(g_payload, TEST_PAYLOAD, g_secret);
    g_image[TEST_PAYLOAD / 2u] ^= 0x5Au;
    serve(g_image, image);

    KT_CHECK_INT_EQ(install("http://test/broken.img", &after), OTA_EDIGEST);
    KT_CHECK_INT_EQ(ota_status(&after), OTA_OK);
    KT_CHECK_INT_EQ(after.active, before.active);
    KT_CHECK_INT_EQ(after.slot_state, OTA_SLOT_VALID);
    KT_CHECK_INT_EQ((int)slot_bytes(inactive_slot(&before), bytes, sizeof(bytes)), (int)size);

    ktest_begin("ota: the signature of another key is refused");

    image = make_image(g_payload, TEST_PAYLOAD, g_other_secret);
    serve(g_image, image);

    KT_CHECK_INT_EQ(install("http://test/other.img", &after), OTA_ESIGN);
    KT_CHECK_INT_EQ(ota_status(&after), OTA_OK);
    KT_CHECK_INT_EQ(after.active, before.active);

    ktest_begin("ota: an image larger than the slot is refused");

    {
        struct ota_config config;

        memset(&config, 0, sizeof(config));
        config.url = "http://test/big.img";
        config.state = OTA_STATE_DEFAULT;
        config.key_path = "/etc/ota/image.pub";
        config.transport = &g_fake;
        config.max_image = 512;

        image = make_image(g_payload, TEST_PAYLOAD, g_secret);
        serve(g_image, image);

        KT_CHECK_INT_EQ(ota_install(&config, &after), OTA_ESIZE);
        KT_CHECK_INT_EQ(ota_status(&after), OTA_OK);
        KT_CHECK_INT_EQ(after.active, before.active);
    }

    ktest_begin("ota: a trailer that is not one is refused");

    serve(g_image, 64);

    KT_CHECK_INT_EQ(install("http://test/short.img", &after), OTA_ESIZE);

    {
        static k_u8_t junk[256];

        memset(junk, 0xA5, sizeof(junk));
        serve(junk, sizeof(junk));

        KT_CHECK_INT_EQ(install("http://test/junk.img", &after), OTA_EFORMAT);
    }

    ktest_begin("ota: a machine without a key takes no image");

    {
        struct ota_config config;

        memset(&config, 0, sizeof(config));
        config.url = "http://test/update.img";
        config.state = OTA_STATE_DEFAULT;
        config.transport = &g_fake;

        image = make_image(g_payload, TEST_PAYLOAD, g_secret);
        serve(g_image, image);

        KT_CHECK_INT_EQ(ota_install(&config, &after), OTA_ENOKEY);
    }

    ktest_begin("ota: a server that answers nothing is a failed fetch");

    g_server.body = g_image;
    g_server.size = 0;
    g_server.sent = 0;
    g_server.answered = false;
    g_fake.user = &g_server;

    KT_CHECK_INT_EQ(install("http://test/empty.img", &after), OTA_ESIZE);
    KT_CHECK_INT_EQ(ota_status(&after), OTA_OK);
    KT_CHECK_INT_EQ(after.active, before.active);
}

void test_ota(void)
{
    if (!make_tree()) {
        KT_CHECK(false);
        return;
    }

    test_install_and_confirm();
    test_rollback();
    test_refusals();

    ktest_begin("ota: the state of the machine survives a read");
    {
        struct ota_state state;
        struct ota_state again;

        KT_CHECK_INT_EQ(ota_status(&state), OTA_OK);
        KT_CHECK_INT_EQ(ota_status(&again), OTA_OK);
        KT_CHECK_INT_EQ(state.active, again.active);
        KT_CHECK_INT_EQ(state.slot_state, again.slot_state);
        KT_CHECK_INT_EQ((int)state.sequence, (int)again.sequence);
    }

    drop_tree();
}
