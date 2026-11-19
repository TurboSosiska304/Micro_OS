/*
 * Micro OS — board description.
 *
 * Static description of the hardware the kernel is running on. Used by the
 * boot banner, by `uname -a` in the shell and by driver probing.
 *
 * The shape of the description is the one of the kernel: it hands the
 * structure over at the boot through kernel/board.h, so the components
 * above the port never see the board itself.
 */
#ifndef MICRO_OS_BOARD_BOARD_H
#define MICRO_OS_BOARD_BOARD_H

#include "kernel/board.h"

/* Description of the board this binary was built for. */
const struct kernel_board *board_get_info(void);

#endif /* MICRO_OS_BOARD_BOARD_H */
