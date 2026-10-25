/*
 * Micro OS — one line of text cut into the rows of a picture.
 *
 * The console of the display has a width in columns, and a line of a
 * person can be longer than it: the tail has to go to the row below,
 * not beyond the right edge — and the width is the one of the picture
 * as it is turned right now, so the same line is cut differently in
 * the portrait and in the landscape. The cutting is pure arithmetic
 * with no display in it, which is why the host tests compile the file
 * themselves and push strings through it.
 */
#ifndef MICRO_OS_GRAPHICS_TEXT_WRAP_H
#define MICRO_OS_GRAPHICS_TEXT_WRAP_H

/*
 * The piece of the line that belongs to the row `row` of the wrap
 * (0 based): the bytes are copied into dst, NUL-terminated, at most
 * dst_max bytes including the NUL. Returns the number of bytes taken,
 * and 0 when there is no such row — the line is over.
 */
int text_wrap_take(char *dst, int dst_max, const char *line, int columns, int row);

#endif /* MICRO_OS_GRAPHICS_TEXT_WRAP_H */
