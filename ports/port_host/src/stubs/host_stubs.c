/*
 * Micro OS — the entry point of the board stubs of the host port.
 *
 * The simulator boots the same portable system as the board, and that system
 * expects the drivers to have registered their modules and commands before
 * init runs the scripts of /etc/init.d. One call does the work; the service
 * level of the boot is the moment it happens.
 */
#include <stdlib.h>

#include "host_stubs.h"
#include "port_host.h"

const char *host_input_script(void)
{
    const char *path = getenv("MICROOS_INPUT");

    return (path != NULL && path[0] != '\0') ? path : NULL;
}

const char *host_screen_path(void)
{
    const char *path = getenv("MICROOS_SCREEN");

    return (path != NULL && path[0] != '\0') ? path : NULL;
}

bool port_host_screen_enabled(void)
{
    return host_screen_path() != NULL;
}

void port_host_stubs_init(void)
{
    host_modules_register();
    host_commands_register();
    host_clock_register();
    host_render_register();
}
