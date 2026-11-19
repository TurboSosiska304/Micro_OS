/*
 * Micro OS — the stubs of the board for the host port, internal interface.
 *
 * The host has no panel, no buttons, no radio and no battery, but the
 * portable system does not know that: it asks the module registry and the
 * shell for the same names the board registers. These helpers keep the
 * answers in one place, so the modules and the commands of the simulator
 * cannot drift apart.
 */
#ifndef MICRO_OS_PORT_HOST_STUBS_H
#define MICRO_OS_PORT_HOST_STUBS_H

#include <stddef.h>

#include "kernel/types.h"

/* The registry of the hardware doctor and the commands of the shell. */
void host_modules_register(void);
void host_commands_register(void);

/* The renderer of the file screen, and the clock of the process. */
void host_render_register(void);
void host_clock_register(void);

/*
 * The wall clock of the host: the clock of the process plus the offset that
 * `date set` and `ntp` move. The kernel owns the offset, this is its door.
 */
k_i64_t host_clock_now_unix(void);
int host_clock_set_unix(k_u64_t unix_seconds);
void host_clock_text(char *buf, size_t size);

/* $MICROOS_INPUT and $MICROOS_SCREEN, NULL when they are unset or empty. */
const char *host_input_script(void);
const char *host_screen_path(void);

#endif /* MICRO_OS_PORT_HOST_STUBS_H */
