/*
 * Micro OS — the registry of the board description.
 *
 * The pointer is the whole state: the description is static on the side of
 * the platform and outlives the kernel, so there is nothing to copy and
 * nothing to free here.
 */
#include "kernel/board.h"

static const struct kernel_board *g_board;

void k_board_register(const struct kernel_board *board)
{
    g_board = board;
}

const struct kernel_board *k_board_get(void)
{
    return g_board;
}
