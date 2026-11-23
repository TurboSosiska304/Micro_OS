/*
 * Micro OS — the protocol of the window server and the client of it
 * (docs/desktop.md).
 *
 * The two halves of `components/ui/client` are portable and this file is the
 * proof: the messages are a struct the two sides compile, and the client is a
 * canvas with a transport. The transport here is two arrays and a counter -
 * the `struct wm_link` of the header - so the test holds the whole of the
 * contract of a window without a pipe, a mapping or a screen:
 *
 *  * the builders fill every field of a message and the title is cut;
 *  * the attach reads the hello of the server and asks the transport for the
 *    buffer of the window;
 *  * a fill and a blit touch the pixels of the buffer, clip at its edge and
 *    grow one damage rectangle;
 *  * a flush makes the buffer visible and then sends the damage - the order
 *    of the two calls is the contract of the shared file - and sends nothing
 *    for a frame that did not change;
 *  * the keys and the close of the server arrive as events of the window.
 */
#include <string.h>

#include "ktest.h"
#include "tests/tests.h"

#include "kernel/input.h"
#include "kernel/types.h"
#include "ui/draw.h"
#include "ui/wm_client.h"
#include "ui/wm_protocol.h"

/* The window of the test and the buffer of it. */
#define TEST_WIDTH 32
#define TEST_HEIGHT 16

/* The transport of the test: the messages the server would send, the messages
 * the client sends, and the count of the two calls that are not messages. */
#define TEST_QUEUE 8

struct test_link {
    struct wm_link link;
    struct wm_message inbox[TEST_QUEUE];
    int inbox_count;
    int inbox_at;
    struct wm_message outbox[TEST_QUEUE];
    int out_count;
    int maps;
    int syncs;
    k_u16_t pixels[TEST_WIDTH * TEST_HEIGHT];
};

static int test_map(void *context, int width, int height, k_u16_t **pixels)
{
    struct test_link *fake = (struct test_link *)context;

    if (width != TEST_WIDTH || height != TEST_HEIGHT) {
        return -1;
    }

    fake->maps++;
    *pixels = fake->pixels;

    return 0;
}

static int test_sync(void *context)
{
    struct test_link *fake = (struct test_link *)context;

    fake->syncs++;

    return 0;
}

static int test_send(void *context, const struct wm_message *message)
{
    struct test_link *fake = (struct test_link *)context;

    if (fake->out_count >= TEST_QUEUE) {
        return -1;
    }

    fake->outbox[fake->out_count++] = *message;

    return 0;
}

static int test_recv(void *context, struct wm_message *message, bool wait)
{
    struct test_link *fake = (struct test_link *)context;

    (void)wait;

    if (fake->inbox_at >= fake->inbox_count) {
        return 0;
    }

    *message = fake->inbox[fake->inbox_at++];

    return 1;
}

static void link_init(struct test_link *fake)
{
    memset(fake, 0, sizeof(*fake));

    fake->link.context = fake;
    fake->link.map = test_map;
    fake->link.sync = test_sync;
    fake->link.send = test_send;
    fake->link.recv = test_recv;
}

static void link_push(struct test_link *fake, const struct wm_message *message)
{
    fake->inbox[fake->inbox_count++] = *message;
}

/* A client attached to a window of the test, with the hello of the server. */
static int client_attach(struct wm_client *client, struct test_link *fake)
{
    struct wm_message hello;

    link_init(fake);
    wm_message_hello(&hello, 100, 50, TEST_WIDTH, TEST_HEIGHT, "test");
    link_push(fake, &hello);

    return wm_client_attach(client, &fake->link);
}

static void test_wm_messages(void)
{
    struct wm_message message;

    ktest_begin("wm: the messages of the window server are one struct");

    /* The bytes of the contract are the size of the struct the two sides
     * compile: a message is one write of a pipe and one read of it. */
    KT_CHECK_INT_EQ((int)sizeof(struct wm_message), (int)WM_MESSAGE_BYTES);

    wm_message_damage(&message, 1, 2, 3, 4);
    KT_CHECK_INT_EQ(message.type, WM_MSG_DAMAGE);
    KT_CHECK_INT_EQ(message.x, 1);
    KT_CHECK_INT_EQ(message.y, 2);
    KT_CHECK_INT_EQ(message.w, 3);
    KT_CHECK_INT_EQ(message.h, 4);
    KT_CHECK_INT_EQ(message.code, 0);
    KT_CHECK_INT_EQ(message.text[0], '\0');

    wm_message_hello(&message, 10, 20, 300, 200, "edit - /doc.txt");
    KT_CHECK_INT_EQ(message.type, WM_MSG_HELLO);
    KT_CHECK_INT_EQ(message.x, 10);
    KT_CHECK_INT_EQ(message.y, 20);
    KT_CHECK_INT_EQ(message.w, 300);
    KT_CHECK_INT_EQ(message.h, 200);
    KT_CHECK_STR_EQ(message.text, "edit - /doc.txt");

    /* A title longer than the field is cut and still a text. */
    {
        static const char long_title[] =
            "012345678901234567890123456789012345678901234567890123456789";

        wm_message_title(&message, long_title);
        KT_CHECK_INT_EQ(message.type, WM_MSG_TITLE);
        KT_CHECK_INT_EQ((int)strlen(message.text), (int)WM_TITLE_MAX - 1);
        KT_CHECK_INT_EQ((int)strlen(message.text), (int)sizeof(message.text) - 1);
    }

    wm_message_focus(&message, true);
    KT_CHECK_INT_EQ(message.type, WM_MSG_FOCUS);
    KT_CHECK_INT_EQ(message.x, 1);
    wm_message_focus(&message, false);
    KT_CHECK_INT_EQ(message.x, 0);

    wm_message_key(&message, K_INPUT_KEY_ENTER, true);
    KT_CHECK_INT_EQ(message.type, WM_MSG_KEY);
    KT_CHECK_INT_EQ(message.code, K_INPUT_KEY_ENTER);
    KT_CHECK_INT_EQ(message.x, 1);
    wm_message_key(&message, K_INPUT_KEY_ENTER, false);
    KT_CHECK_INT_EQ(message.x, 0);

    wm_message_motion(&message, 7, 9);
    KT_CHECK_INT_EQ(message.type, WM_MSG_MOTION);
    KT_CHECK_INT_EQ(message.x, 7);
    KT_CHECK_INT_EQ(message.y, 9);

    wm_message_button(&message, 5, 6, true);
    KT_CHECK_INT_EQ(message.type, WM_MSG_BUTTON);
    KT_CHECK_INT_EQ(message.x, 1);
    KT_CHECK_INT_EQ(message.y, 5);
    KT_CHECK_INT_EQ(message.w, 6);

    wm_message_close(&message);
    KT_CHECK_INT_EQ(message.type, WM_MSG_CLOSE);

    /* The direction of a type: a request goes up, an event comes down. */
    KT_CHECK_INT_EQ(wm_message_is_event(WM_MSG_HELLO), true);
    KT_CHECK_INT_EQ(wm_message_is_event(WM_MSG_KEY), true);
    KT_CHECK_INT_EQ(wm_message_is_event(WM_MSG_CLOSE), true);
    KT_CHECK_INT_EQ(wm_message_is_event(WM_MSG_DAMAGE), false);
    KT_CHECK_INT_EQ(wm_message_is_event(WM_MSG_TITLE), false);

    KT_CHECK_INT_EQ(wm_message_known(&message), true);
    message.type = WM_MSG_TYPE_MAX + 1u;
    KT_CHECK_INT_EQ(wm_message_known(&message), false);
    message.type = WM_MSG_NONE;
    KT_CHECK_INT_EQ(wm_message_known(&message), false);
}

static void test_wm_client_attach(void)
{
    struct wm_client client;
    struct test_link fake;

    ktest_begin("wm: the client attaches to the window the server gave it");

    KT_CHECK_INT_EQ(client_attach(&client, &fake), 0);
    KT_CHECK_INT_EQ(fake.maps, 1);
    KT_CHECK_INT_EQ(client.width, TEST_WIDTH);
    KT_CHECK_INT_EQ(client.height, TEST_HEIGHT);
    KT_CHECK_INT_EQ(client.geometry.x, 100);
    KT_CHECK_INT_EQ(client.geometry.y, 50);
    KT_CHECK_INT_EQ(client.geometry.w, TEST_WIDTH);
    KT_CHECK_INT_EQ(client.geometry.h, TEST_HEIGHT);
    KT_CHECK_STR_EQ(client.title, "test");
    KT_CHECK_INT_EQ(client.pixels, fake.pixels);
    KT_CHECK_INT_EQ(client.target.fill, wm_client_fill);
    KT_CHECK_INT_EQ(client.target.blit, wm_client_blit);
    KT_CHECK_INT_EQ(client.damaged, false);

    /* A window without a hello is a window the server did not give. */
    link_init(&fake);
    KT_CHECK_INT_EQ(wm_client_attach(&client, &fake.link) < 0, true);

    /* A hello with no rectangle is not one either. */
    link_init(&fake);
    {
        struct wm_message hello;

        wm_message_hello(&hello, 0, 0, 0, 0, "empty");
        link_push(&fake, &hello);
    }
    KT_CHECK_INT_EQ(wm_client_attach(&client, &fake.link) < 0, true);
}

static void test_wm_client_paint(void)
{
    struct wm_client client;
    struct test_link fake;
    struct ui_rect rect;
    k_u16_t row[8];

    ktest_begin("wm: a fill and a blit of the client are the buffer and a damage");

    (void)client_attach(&client, &fake);

    KT_CHECK_INT_EQ(wm_client_fill(&client, &(struct ui_rect){2, 3, 4, 5}, 0x1234u), 0);
    KT_CHECK_INT_EQ(client.pixels[3 * TEST_WIDTH + 2], 0x1234u);
    KT_CHECK_INT_EQ(client.pixels[7 * TEST_WIDTH + 5], 0x1234u);
    KT_CHECK_INT_EQ(client.pixels[3 * TEST_WIDTH + 1], 0);
    KT_CHECK_INT_EQ(client.damaged, true);
    KT_CHECK_INT_EQ(client.damage.x, 2);
    KT_CHECK_INT_EQ(client.damage.y, 3);
    KT_CHECK_INT_EQ(client.damage.w, 4);
    KT_CHECK_INT_EQ(client.damage.h, 5);

    /* A second rectangle grows the one damage of the frame. */
    KT_CHECK_INT_EQ(wm_client_fill(&client, &(struct ui_rect){10, 1, 2, 2}, 0x4321u), 0);
    KT_CHECK_INT_EQ(client.damage.x, 2);
    KT_CHECK_INT_EQ(client.damage.y, 1);
    KT_CHECK_INT_EQ(client.damage.w, 10);
    KT_CHECK_INT_EQ(client.damage.h, 7);

    /* A rectangle that leaves the window is cut, not refused: the canvas of
     * a program composes and the device of ADR 0028 is the one that refuses. */
    client.damaged = false;
    KT_CHECK_INT_EQ(wm_client_fill(&client, &(struct ui_rect){-4, -4, 8, 8}, 0x00FFu), 0);
    KT_CHECK_INT_EQ(client.damage.x, 0);
    KT_CHECK_INT_EQ(client.damage.y, 0);
    KT_CHECK_INT_EQ(client.damage.w, 4);
    KT_CHECK_INT_EQ(client.damage.h, 4);
    KT_CHECK_INT_EQ(client.pixels[0], 0x00FFu);

    /* Nothing outside the window was written. */
    KT_CHECK_INT_EQ(client.pixels[(TEST_HEIGHT - 1) * TEST_WIDTH + (TEST_WIDTH - 1)], 0);

    /* A blit of a canvas: the source is the rectangle it was given, so the
     * part of it that is inside the window is read at the offset of that
     * part and the part that leaves the right edge is not written. */
    memset(row, 0, sizeof(row));
    row[0] = 0xAAAAu;
    row[3] = 0xBBBBu;
    row[4] = 0xCCCCu;
    row[7] = 0xDDDDu;
    rect.x = TEST_WIDTH - 4;
    rect.y = 0;
    rect.w = 8;
    rect.h = 1;
    KT_CHECK_INT_EQ(wm_client_blit(&client, &rect, row), 0);
    KT_CHECK_INT_EQ(client.pixels[TEST_WIDTH - 4], 0xAAAAu);
    KT_CHECK_INT_EQ(client.pixels[TEST_WIDTH - 1], 0xBBBBu);
    KT_CHECK_INT_EQ(client.damage.x, 0);
    KT_CHECK_INT_EQ(client.damage.w, TEST_WIDTH);

    /* The same rectangle inside the window takes its whole row. */
    rect.x = 0;
    KT_CHECK_INT_EQ(wm_client_blit(&client, &rect, row), 0);
    KT_CHECK_INT_EQ(client.pixels[0], 0xAAAAu);
    KT_CHECK_INT_EQ(client.pixels[3], 0xBBBBu);
    KT_CHECK_INT_EQ(client.pixels[4], 0xCCCCu);
    KT_CHECK_INT_EQ(client.pixels[7], 0xDDDDu);

    /* A rectangle wholly outside the window changes nothing. */
    client.damaged = false;
    KT_CHECK_INT_EQ(wm_client_fill(&client, &(struct ui_rect){-20, 0, 4, 4}, 0x1111u), 0);
    KT_CHECK_INT_EQ(client.damaged, false);

    /* The whole window in one colour. */
    KT_CHECK_INT_EQ(wm_client_clear(&client, 0x2222u), 0);
    KT_CHECK_INT_EQ(client.pixels[0], 0x2222u);
    KT_CHECK_INT_EQ(client.pixels[TEST_WIDTH * TEST_HEIGHT - 1], 0x2222u);
    KT_CHECK_INT_EQ(client.damage.w, TEST_WIDTH);
    KT_CHECK_INT_EQ(client.damage.h, TEST_HEIGHT);
}

static void test_wm_client_flush(void)
{
    struct wm_client client;
    struct test_link fake;

    ktest_begin("wm: a frame is the buffer made visible and then the damage");

    (void)client_attach(&client, &fake);

    /* A frame that did not change sends nothing and does not sync: a client
     * of a still picture is a client off the pipe. */
    KT_CHECK_INT_EQ(wm_client_flush(&client), 0);
    KT_CHECK_INT_EQ(fake.out_count, 0);
    KT_CHECK_INT_EQ(fake.syncs, 0);

    (void)wm_client_fill(&client, &(struct ui_rect){1, 1, 2, 3}, 0x5555u);
    KT_CHECK_INT_EQ(wm_client_flush(&client), 1);

    /* The mapping is made visible before the message that says it changed. */
    KT_CHECK_INT_EQ(fake.syncs, 1);
    KT_CHECK_INT_EQ(fake.out_count, 1);
    KT_CHECK_INT_EQ(fake.outbox[0].type, WM_MSG_DAMAGE);
    KT_CHECK_INT_EQ(fake.outbox[0].x, 1);
    KT_CHECK_INT_EQ(fake.outbox[0].y, 1);
    KT_CHECK_INT_EQ(fake.outbox[0].w, 2);
    KT_CHECK_INT_EQ(fake.outbox[0].h, 3);
    KT_CHECK_INT_EQ(client.frames, 1);
    KT_CHECK_INT_EQ(client.damaged, false);

    /* The second frame is one more sync and one more message. */
    (void)wm_client_fill(&client, &(struct ui_rect){0, 0, 1, 1}, 0x5555u);
    KT_CHECK_INT_EQ(wm_client_flush(&client), 1);
    KT_CHECK_INT_EQ(fake.syncs, 2);
    KT_CHECK_INT_EQ(fake.out_count, 2);

    /* The title: one message when it changes and none when it is the same. */
    KT_CHECK_INT_EQ(wm_client_set_title(&client, "edit - /doc.txt"), 0);
    KT_CHECK_INT_EQ(fake.out_count, 3);
    KT_CHECK_INT_EQ(fake.outbox[2].type, WM_MSG_TITLE);
    KT_CHECK_STR_EQ(fake.outbox[2].text, "edit - /doc.txt");
    KT_CHECK_STR_EQ(client.title, "edit - /doc.txt");
    KT_CHECK_INT_EQ(wm_client_set_title(&client, "edit - /doc.txt"), 0);
    KT_CHECK_INT_EQ(fake.out_count, 3);

    /* The window says it is leaving. */
    KT_CHECK_INT_EQ(wm_client_request_close(&client), 0);
    KT_CHECK_INT_EQ(fake.out_count, 4);
    KT_CHECK_INT_EQ(fake.outbox[3].type, WM_MSG_CLOSE);
}

static void test_wm_client_events(void)
{
    struct wm_client client;
    struct test_link fake;
    struct input_event event;
    struct wm_message message;

    ktest_begin("wm: the events of the window are the keys and the pointer");

    (void)client_attach(&client, &fake);

    /* The hello was read at the attach and is not an event. */
    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 0);

    wm_message_motion(&message, 11, 12);
    link_push(&fake, &message);
    wm_message_button(&message, 13, 14, true);
    link_push(&fake, &message);

    /* One call takes what the transport has: the pointer is where the last
     * message of it put it, and the button is the edge of the last one. */
    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 0);
    KT_CHECK_INT_EQ(client.pointer_x, 13);
    KT_CHECK_INT_EQ(client.pointer_y, 14);
    KT_CHECK_INT_EQ(client.button, true);

    /* A key is the event of the keyboard a program knows. */
    wm_message_key(&message, K_INPUT_KEY_H, true);
    link_push(&fake, &message);
    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 1);
    KT_CHECK_INT_EQ(event.code, K_INPUT_KEY_H);
    KT_CHECK_INT_EQ(event.value, INPUT_VALUE_PRESS);

    wm_message_key(&message, K_INPUT_KEY_H, false);
    link_push(&fake, &message);
    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 1);
    KT_CHECK_INT_EQ(event.value, INPUT_VALUE_RELEASE);

    /* The focus, the close and a type this generation does not know. */
    wm_message_focus(&message, true);
    link_push(&fake, &message);
    message.type = WM_MSG_TYPE_MAX + 7u;
    link_push(&fake, &message);
    wm_message_close(&message);
    link_push(&fake, &message);

    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 0);
    KT_CHECK_INT_EQ(client.focused, true);
    KT_CHECK_INT_EQ(client.closing, true);
    KT_CHECK_INT_EQ(wm_client_closing(&client), true);
    KT_CHECK_INT_EQ(client.events >= 7u, true);

    /* And a transport with nothing left answers "silent" and changes
     * nothing. */
    KT_CHECK_INT_EQ(wm_client_poll(&client, &event), 0);
    KT_CHECK_INT_EQ(client.events >= 7u, true);
}

void test_ui_wm(void)
{
    test_wm_messages();
    test_wm_client_attach();
    test_wm_client_paint();
    test_wm_client_flush();
    test_wm_client_events();
}
