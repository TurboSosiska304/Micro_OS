/*
 * Micro OS — the attributes of a terminal: the SGR sequences of ANSI.
 *
 * A console of this system is a grid of cells and a cell has a character and
 * a colour. The colours a command asks for arrive as the SGR sequences of a
 * terminal (ECMA-48, "Select Graphic Rendition"): ESC [ 1 ; 31 m, and the
 * next characters are written in the colours the list chose. The state below
 * is the whole of that part of the protocol - the ink, the paper, the bold
 * and the reverse - and it is portable C: a screen of the board, the
 * framebuffer of x86-64 and a test of the host all read it the same way.
 *
 * The numbers of the colours are the sixteen of the CGA/VGA palette, which is
 * the pair a VGA attribute names; a colour of the terminal is one of them and
 * not an RGB value, and the parser translates the order of a terminal (black,
 * red, green, yellow, ...) into the order of the palette (black, blue, green,
 * cyan, red, ...). "The colour of the console" - the theme of /etc - is
 * `K_ANSI_DEFAULT`, a value the palette does not have, so plain text follows
 * a pair a person chose while a command's colour stays the colour it asked
 * for.
 */
#ifndef MICRO_OS_KERNEL_ANSI_H
#define MICRO_OS_KERNEL_ANSI_H

#include "kernel/types.h"

/* The value of `fg` and `bg` that means the pair of the console, not a
 * palette entry: the theme of /etc/console picks it. */
#define K_ANSI_DEFAULT 0xFFu

/* The colours of the sixteen attributes of the VGA. */
#define K_ANSI_COLORS 16u

struct k_ansi {
    k_u8_t fg; /* 0..15, or K_ANSI_DEFAULT */
    k_u8_t bg; /* 0..15, or K_ANSI_DEFAULT */
    bool bold;
    bool reverse;
};

/*
 * The state of a terminal that has been sent ESC[0m: the pair of the console
 * and neither a bold nor a reversed line.
 */
void k_ansi_reset(struct k_ansi *state);

/*
 * Apply one parameter of an SGR sequence. Returns true when the parameter is
 * one this system knows and the state changed, false for the ones it ignores
 * (2, 4, 5, ...): an unknown parameter of a sequence is skipped and the rest
 * of the list still counts, which is what a terminal does.
 */
bool k_ansi_sgr(struct k_ansi *state, int parameter);

/*
 * Apply a whole parameter list: the bytes between the "ESC[" and the final
 * 'm', digits and semicolons. An empty list is the reset (ESC[m is ESC[0m),
 * an empty parameter between two semicolons is a zero, and a list that is
 * longer than a line of a terminal is cut at a parameter boundary. Returns
 * the number of parameters the state took.
 */
int k_ansi_sgr_text(struct k_ansi *state, const char *text, k_size_t len);

/*
 * The attribute of one cell for this state: the ink in the low nibble and the
 * paper in the high one, the byte a VGA calls (background << 4) | foreground.
 * `plain_fg` and `plain_bg` are the pair of the console, used by the text
 * that named no colour of its own. A bold line brightens an ink it asked for
 * (30..37 become 90..97); the pair of the console is the colour the theme
 * chose and is not brightened.
 */
k_u8_t k_ansi_attr(const struct k_ansi *state, k_u8_t plain_fg, k_u8_t plain_bg);

#endif /* MICRO_OS_KERNEL_ANSI_H */
