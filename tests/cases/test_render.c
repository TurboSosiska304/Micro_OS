/*
 * Micro OS — the picture of the system.
 *
 * The console of the display draws through a renderer, and a board with no
 * display has none: the registry is the whole of the contract between the
 * two, so the test walks it the way the boot does - nothing, then a
 * renderer, then nothing again.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/render.h"

static int s_clears;
static int s_texts;
static k_u16_t s_last_color;
static char s_last_text[16];

static int fake_width(void *ctx)
{
    K_UNUSED(ctx);
    return 135;
}

static int fake_height(void *ctx)
{
    K_UNUSED(ctx);
    return 240;
}

static void fake_clear(void *ctx, k_u16_t color)
{
    K_UNUSED(ctx);
    s_clears++;
    s_last_color = color;
}

static void fake_text(void *ctx, int x, int y, const char *text, k_u16_t color)
{
    K_UNUSED(ctx);
    K_UNUSED(x);
    K_UNUSED(y);
    s_texts++;
    s_last_color = color;

    if (text != NULL) {
        k_size_t len = strlen(text);

        if (len >= sizeof(s_last_text)) {
            len = sizeof(s_last_text) - 1;
        }
        memcpy(s_last_text, text, len);
        s_last_text[len] = '\0';
    }
}

static void fake_rect(void *ctx, int x, int y, int w, int h, k_u16_t color)
{
    K_UNUSED(ctx);
    K_UNUSED(x);
    K_UNUSED(y);
    K_UNUSED(w);
    K_UNUSED(h);
    K_UNUSED(color);
}

static const struct k_renderer s_renderer = {
    .name = "fake",
    .width = fake_width,
    .height = fake_height,
    .clear = fake_clear,
    .text = fake_text,
    .fill_rect = fake_rect,
    .rect = fake_rect,
    .line = NULL,
    .flush = NULL,
    .ctx = NULL,
};

void test_render(void)
{
    const struct k_renderer *picture;

    ktest_begin("render");

    /* The host and a board without a panel have no picture at all. */
    KT_CHECK(k_render_get() == NULL);

    k_render_register(&s_renderer);
    picture = k_render_get();
    KT_CHECK(picture == &s_renderer);
    KT_CHECK(picture != NULL && strcmp(picture->name, "fake") == 0);

    KT_CHECK_INT_EQ(picture->width(picture->ctx), 135);
    KT_CHECK_INT_EQ(picture->height(picture->ctx), 240);

    s_clears = 0;
    s_texts = 0;
    s_last_color = 0;
    s_last_text[0] = '\0';

    picture->clear(picture->ctx, 0x1234);
    KT_CHECK_INT_EQ(s_clears, 1);
    KT_CHECK_INT_EQ(s_last_color, 0x1234);

    picture->text(picture->ctx, 0, 0, "hello", 0x0F0F);
    KT_CHECK_INT_EQ(s_texts, 1);
    KT_CHECK_INT_EQ(s_last_color, 0x0F0F);
    KT_CHECK_STR_EQ(s_last_text, "hello");

    /* A console on a board without a display is not an error: it simply
     * has nothing to draw on, and the picture can be taken away. */
    k_render_register(NULL);
    KT_CHECK(k_render_get() == NULL);
}
