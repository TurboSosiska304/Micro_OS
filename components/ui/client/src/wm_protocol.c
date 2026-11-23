/*
 * Micro OS — the messages of the window server (ui/wm_protocol.h).
 *
 * Every function fills one struct and touches nothing else: the protocol is
 * the shape of the messages, and a test on the host holds it without a pipe
 * (tests/cases/test_ui_wm.c). The builders set every field, so a message
 * carries the zeros of the fields its type does not use and never the bytes
 * of the message before it.
 */
#include "ui/wm_protocol.h"

#include <string.h>

static void message_begin(struct wm_message *message, k_u16_t type)
{
    if (message == NULL) {
        return;
    }

    memset(message, 0, sizeof(*message));
    message->type = type;
}

void wm_text_copy(char *destination, k_size_t size, const char *source)
{
    k_size_t length = 0;

    if (destination == NULL || size == 0u) {
        return;
    }

    while (source != NULL && source[length] != '\0' && length + 1u < size) {
        destination[length] = source[length];
        length++;
    }

    destination[length] = '\0';
}

void wm_message_damage(struct wm_message *message, int x, int y, int w, int h)
{
    message_begin(message, WM_MSG_DAMAGE);

    if (message == NULL) {
        return;
    }

    message->x = x;
    message->y = y;
    message->w = w;
    message->h = h;
}

void wm_message_title(struct wm_message *message, const char *title)
{
    message_begin(message, WM_MSG_TITLE);

    if (message == NULL || title == NULL) {
        return;
    }

    /* The text is cut, never refused: a title too long for the bar is a title
     * a person reads the beginning of. */
    wm_text_copy(message->text, sizeof(message->text), title);
}

void wm_message_close(struct wm_message *message)
{
    message_begin(message, WM_MSG_CLOSE);
}

void wm_message_hello(struct wm_message *message, int x, int y, int w, int h, const char *title)
{
    message_begin(message, WM_MSG_HELLO);

    if (message == NULL) {
        return;
    }

    message->x = x;
    message->y = y;
    message->w = w;
    message->h = h;

    if (title != NULL) {
        wm_text_copy(message->text, sizeof(message->text), title);
    }
}

void wm_message_focus(struct wm_message *message, bool focused)
{
    message_begin(message, WM_MSG_FOCUS);

    if (message == NULL) {
        return;
    }

    message->x = focused ? 1 : 0;
}

void wm_message_key(struct wm_message *message, k_u16_t code, bool pressed)
{
    message_begin(message, WM_MSG_KEY);

    if (message == NULL) {
        return;
    }

    message->code = code;
    message->x = pressed ? 1 : 0;
}

void wm_message_motion(struct wm_message *message, int x, int y)
{
    message_begin(message, WM_MSG_MOTION);

    if (message == NULL) {
        return;
    }

    message->x = x;
    message->y = y;
}

void wm_message_button(struct wm_message *message, int x, int y, bool pressed)
{
    message_begin(message, WM_MSG_BUTTON);

    if (message == NULL) {
        return;
    }

    /* The fields of the type: `x` is the edge, `y` and `w` the point in the
     * window - a message is one struct and a field is a name of the type
     * that writes it, the way `text` is the title of another. */
    message->x = pressed ? 1 : 0;
    message->y = x;
    message->w = y;
}

bool wm_message_known(const struct wm_message *message)
{
    if (message == NULL) {
        return false;
    }

    return message->type != WM_MSG_NONE && message->type <= WM_MSG_TYPE_MAX;
}

bool wm_message_is_event(k_u16_t type)
{
    switch (type) {
    case WM_MSG_HELLO:
    case WM_MSG_FOCUS:
    case WM_MSG_KEY:
    case WM_MSG_MOTION:
    case WM_MSG_BUTTON:
    case WM_MSG_CLOSE:
        return true;
    default:
        return false;
    }
}
