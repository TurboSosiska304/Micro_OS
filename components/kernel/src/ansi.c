/*
 * The SGR state of a terminal (kernel/ansi.h).
 *
 * The parser is the table of ECMA-48 that a console needs and nothing more:
 * a reset, the bold, the reverse, the eight colours of the text and of the
 * paper and their bright forms. Every other parameter is ignored, which is
 * what a terminal does with a rendition it does not have - dropping the whole
 * sequence instead would lose the colour of the parameters after it.
 *
 * The numbers of a terminal and the numbers of a VGA are not the same order:
 * a terminal names its colours black, red, green, yellow, blue, magenta, cyan,
 * white, and the two bits of a CGA attribute are black, blue, green, cyan,
 * red, magenta, brown, light grey. The table below is that translation, so the
 * state holds the index of the palette of the screen and 33 (the yellow of a
 * warning) is the amber the sixteen colours actually have.
 */
#include "kernel/ansi.h"

/* The colour of a terminal (0..7) and the index of the CGA palette. */
static const k_u8_t s_ansi_to_palette[8] = {
    0, /* 30 black */
    4, /* 31 red */
    2, /* 32 green */
    6, /* 33 yellow, the brown of the palette */
    1, /* 34 blue */
    5, /* 35 magenta */
    3, /* 36 cyan */
    7, /* 37 white, the light grey of the palette */
};

void k_ansi_reset(struct k_ansi *state)
{
    if (state == NULL) {
        return;
    }

    state->fg = K_ANSI_DEFAULT;
    state->bg = K_ANSI_DEFAULT;
    state->bold = false;
    state->reverse = false;
}

bool k_ansi_sgr(struct k_ansi *state, int parameter)
{
    if (state == NULL) {
        return false;
    }

    if (parameter == 0) {
        k_ansi_reset(state);
        return true;
    }

    switch (parameter) {
    case 1:
        state->bold = true;
        return true;
    case 22:
        state->bold = false;
        return true;
    case 7:
        state->reverse = true;
        return true;
    case 27:
        state->reverse = false;
        return true;
    case 39:
        state->fg = K_ANSI_DEFAULT;
        return true;
    case 49:
        state->bg = K_ANSI_DEFAULT;
        return true;
    default:
        break;
    }

    /* The eight colours of the text, the eight of the paper, and the bright
     * eight of each: the numbers are the ones of every terminal, translated
     * to the order of the palette of a CGA attribute. */
    if (parameter >= 30 && parameter <= 37) {
        state->fg = s_ansi_to_palette[parameter - 30];
        return true;
    }
    if (parameter >= 40 && parameter <= 47) {
        state->bg = s_ansi_to_palette[parameter - 40];
        return true;
    }
    if (parameter >= 90 && parameter <= 97) {
        state->fg = (k_u8_t)(s_ansi_to_palette[parameter - 90] + 8u);
        return true;
    }
    if (parameter >= 100 && parameter <= 107) {
        state->bg = (k_u8_t)(s_ansi_to_palette[parameter - 100] + 8u);
        return true;
    }

    return false;
}

int k_ansi_sgr_text(struct k_ansi *state, const char *text, k_size_t len)
{
    k_size_t at = 0;
    int applied = 0;

    if (state == NULL) {
        return 0;
    }

    if (text == NULL || len == 0) {
        (void)k_ansi_sgr(state, 0);
        return 1;
    }

    /* One parameter at a time; a value is capped where a screen of this size
     * stops being a terminal, so a number cannot run over the type. */
    while (at < len) {
        int value = 0;
        bool digits = false;

        while (at < len && text[at] != ';') {
            if (text[at] >= '0' && text[at] <= '9') {
                if (value < 10000) {
                    value = value * 10 + (text[at] - '0');
                }
                digits = true;
            }
            at++;
        }

        if (!digits) {
            value = 0; /* an empty parameter is a zero */
        }

        if (k_ansi_sgr(state, value)) {
            applied++;
        }

        at++; /* the ';', or one past the end of the list */
    }

    /* A list that ends with a semicolon has an empty parameter at its end,
     * which is a zero: ESC[31;m is the reset and then the red. */
    if (len > 0 && text[len - 1] == ';' && k_ansi_sgr(state, 0)) {
        applied++;
    }

    return applied;
}

k_u8_t k_ansi_attr(const struct k_ansi *state, k_u8_t plain_fg, k_u8_t plain_bg)
{
    k_u8_t fg;
    k_u8_t bg;

    if (state == NULL) {
        return (k_u8_t)(((plain_bg & 0x0Fu) << 4) | (plain_fg & 0x0Fu));
    }

    fg = (state->fg == K_ANSI_DEFAULT) ? plain_fg : state->fg;
    bg = (state->bg == K_ANSI_DEFAULT) ? plain_bg : state->bg;

    /* The bold of a terminal is the bright half of the palette. The pair of
     * the console is the theme a person chose and not a colour of a command,
     * so it keeps the shade it was given. */
    if (state->bold && state->fg != K_ANSI_DEFAULT && fg < 8u) {
        fg = (k_u8_t)(fg + 8u);
    }

    if (state->reverse) {
        k_u8_t swap = fg;

        fg = bg;
        bg = swap;
    }

    return (k_u8_t)(((bg & 0x0Fu) << 4) | (fg & 0x0Fu));
}
