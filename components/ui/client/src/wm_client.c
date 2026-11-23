/*
 * Micro OS — the window of a program (ui/wm_client.h).
 *
 * The picture of a window is an array of RGB565 pixels of the caller and the
 * damage is the bounding box of what the program painted. The clipping is
 * the rule of a canvas and not the rule of the device: a program that draws a
 * row of text longer than its window has the row cut, not the write refused
 * (ADR 0028 refuses a rectangle that leaves the screen, and the server of the
 * desktop is the one that owns the screen there).
 */
#include "ui/wm_client.h"

#include <string.h>

/* The pipe carries the struct itself: the number of bytes of the contract is
 * the size of the message the two sides compile. */
_Static_assert(sizeof(struct wm_message) == WM_MESSAGE_BYTES,
               "the message of the window server changed its size");

static bool client_ready(const struct wm_client *client)
{
    return client != NULL && client->link != NULL && client->pixels != NULL && client->width > 0 &&
           client->height > 0;
}

/* The part of `rect` that is inside the window, false when there is none. */
static bool client_clip(const struct wm_client *client, const struct ui_rect *rect,
                        struct ui_rect *out)
{
    int left;
    int top;
    int right;
    int bottom;

    if (client == NULL || rect == NULL || out == NULL) {
        return false;
    }

    left = rect->x < 0 ? 0 : rect->x;
    top = rect->y < 0 ? 0 : rect->y;
    right = rect->x + rect->w;
    bottom = rect->y + rect->h;

    if (right > client->width) {
        right = client->width;
    }
    if (bottom > client->height) {
        bottom = client->height;
    }

    if (right <= left || bottom <= top) {
        return false;
    }

    out->x = left;
    out->y = top;
    out->w = right - left;
    out->h = bottom - top;

    return true;
}

/* The damage of a paint: the bounding box of the old damage and the new
 * rectangle, clipped to the window. A bounding box may name pixels that did
 * not change; the compositor of the server repaints a rectangle and not a
 * list, and a frame of a window is never the whole picture of the machine. */
static void client_damage(struct wm_client *client, const struct ui_rect *rect)
{
    int left;
    int top;
    int right;
    int bottom;

    if (client == NULL || rect == NULL) {
        return;
    }

    left = rect->x;
    top = rect->y;
    right = rect->x + rect->w;
    bottom = rect->y + rect->h;

    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > client->width) {
        right = client->width;
    }
    if (bottom > client->height) {
        bottom = client->height;
    }

    if (right <= left || bottom <= top) {
        return;
    }

    if (!client->damaged) {
        client->damage.x = left;
        client->damage.y = top;
        client->damage.w = right - left;
        client->damage.h = bottom - top;
        client->damaged = true;
        return;
    }

    {
        int damage_right = client->damage.x + client->damage.w;
        int damage_bottom = client->damage.y + client->damage.h;

        if (left < client->damage.x) {
            client->damage.x = left;
        }
        if (top < client->damage.y) {
            client->damage.y = top;
        }
        if (right > damage_right) {
            damage_right = right;
        }
        if (bottom > damage_bottom) {
            damage_bottom = bottom;
        }

        client->damage.w = damage_right - client->damage.x;
        client->damage.h = damage_bottom - client->damage.y;
    }
}

int wm_client_fill(void *context, const struct ui_rect *rect, k_u16_t color)
{
    struct wm_client *client = (struct wm_client *)context;
    struct ui_rect clipped;
    int row;
    int column;

    if (!client_ready(client) || !client_clip(client, rect, &clipped)) {
        return 0;
    }

    for (row = 0; row < clipped.h; row++) {
        k_u16_t *line = client->pixels + (k_size_t)(clipped.y + row) * (k_size_t)client->width +
                        (k_size_t)clipped.x;

        for (column = 0; column < clipped.w; column++) {
            line[column] = color;
        }
    }

    client_damage(client, &clipped);

    return 0;
}

int wm_client_blit(void *context, const struct ui_rect *rect, const k_u16_t *pixels)
{
    struct wm_client *client = (struct wm_client *)context;
    struct ui_rect clipped;
    int row;
    int column;

    if (!client_ready(client) || rect == NULL || pixels == NULL) {
        return -1;
    }

    if (rect->w <= 0 || rect->h <= 0) {
        return 0;
    }

    if (!client_clip(client, rect, &clipped)) {
        return 0;
    }

    /* A clipped rectangle reads the source at the offsets of the rectangle it
     * was given: the caller's pixels are the whole of `rect`. */
    for (row = 0; row < clipped.h; row++) {
        const k_u16_t *source = pixels + (k_size_t)(clipped.y - rect->y + row) * (k_size_t)rect->w +
                                (k_size_t)(clipped.x - rect->x);
        k_u16_t *line = client->pixels + (k_size_t)(clipped.y + row) * (k_size_t)client->width +
                        (k_size_t)clipped.x;

        for (column = 0; column < clipped.w; column++) {
            line[column] = source[column];
        }
    }

    client_damage(client, &clipped);

    return 0;
}

int wm_client_clear(struct wm_client *client, k_u16_t color)
{
    struct ui_rect whole;

    if (client == NULL) {
        return -1;
    }

    whole.x = 0;
    whole.y = 0;
    whole.w = client->width;
    whole.h = client->height;

    return wm_client_fill(client, &whole, color);
}

int wm_client_attach(struct wm_client *client, struct wm_link *link)
{
    struct wm_message hello;
    int rc;

    if (client == NULL || link == NULL || link->recv == NULL || link->map == NULL ||
        link->send == NULL) {
        return -1;
    }

    memset(client, 0, sizeof(*client));
    client->link = link;

    rc = link->recv(link->context, &hello, true);
    if (rc <= 0) {
        return rc < 0 ? rc : -1;
    }

    if (hello.type != WM_MSG_HELLO || hello.w <= 0 || hello.h <= 0) {
        return -1;
    }

    client->width = hello.w;
    client->height = hello.h;
    client->geometry.x = hello.x;
    client->geometry.y = hello.y;
    client->geometry.w = hello.w;
    client->geometry.h = hello.h;
    wm_text_copy(client->title, sizeof(client->title), hello.text);

    rc = link->map(link->context, hello.w, hello.h, &client->pixels);
    if (rc != 0 || client->pixels == NULL) {
        client->pixels = NULL;
        return rc != 0 ? rc : -1;
    }

    client->target.context = client;
    client->target.fill = wm_client_fill;
    client->target.blit = wm_client_blit;

    return 0;
}

int wm_client_flush(struct wm_client *client)
{
    struct wm_message message;
    int rc;

    if (client == NULL || client->link == NULL) {
        return -1;
    }

    if (!client->damaged) {
        return 0;
    }

    /* The bytes of the buffer before the message that says they changed: the
     * server reads the file when the message arrives, so the order of the two
     * calls is the contract. */
    if (client->link->sync != NULL) {
        rc = client->link->sync(client->link->context);
        if (rc < 0) {
            return rc;
        }
    }

    wm_message_damage(&message, client->damage.x, client->damage.y, client->damage.w,
                      client->damage.h);

    rc = client->link->send(client->link->context, &message);
    if (rc < 0) {
        return rc;
    }

    client->damaged = false;
    client->frames++;

    return 1;
}

int wm_client_set_title(struct wm_client *client, const char *title)
{
    struct wm_message message;

    if (client == NULL || client->link == NULL || title == NULL) {
        return -1;
    }

    if (strncmp(client->title, title, sizeof(client->title)) == 0) {
        return 0;
    }

    wm_text_copy(client->title, sizeof(client->title), title);

    wm_message_title(&message, client->title);

    return client->link->send(client->link->context, &message);
}

int wm_client_request_close(struct wm_client *client)
{
    struct wm_message message;

    if (client == NULL || client->link == NULL || client->link->send == NULL) {
        return -1;
    }

    wm_message_close(&message);

    return client->link->send(client->link->context, &message);
}

int wm_client_poll(struct wm_client *client, struct input_event *event)
{
    for (;;) {
        struct wm_message message;
        int rc;

        rc = client->link->recv(client->link->context, &message, false);
        if (rc <= 0) {
            return rc;
        }

        client->events++;

        if (!wm_message_known(&message)) {
            continue;
        }

        switch (message.type) {
        case WM_MSG_KEY:
            if (event == NULL) {
                continue;
            }

            event->ms = 0;
            event->code = message.code;
            event->value = message.x != 0 ? INPUT_VALUE_PRESS : INPUT_VALUE_RELEASE;
            return 1;
        case WM_MSG_MOTION:
            client->pointer_x = message.x;
            client->pointer_y = message.y;
            break;
        case WM_MSG_BUTTON:
            client->button = message.x != 0;
            client->pointer_x = message.y;
            client->pointer_y = message.w;
            break;
        case WM_MSG_FOCUS:
            client->focused = message.x != 0;
            break;
        case WM_MSG_CLOSE:
            client->closing = true;
            break;
        default:
            /* The hello was read at the attach; a request is not an event of a
             * window and is dropped. */
            break;
        }
    }
}

bool wm_client_closing(const struct wm_client *client)
{
    return client != NULL && client->closing;
}
