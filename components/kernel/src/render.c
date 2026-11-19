/*
 * Micro OS — the picture of the system.
 *
 * One pointer, written by the driver of the panel during the boot and read
 * by the console afterwards. The two do not overlap in time, so no lock is
 * needed; the pointer is written once and only read afterwards.
 */
#include "kernel/render.h"

static const struct k_renderer *g_renderer;

void k_render_register(const struct k_renderer *renderer)
{
    g_renderer = renderer;
}

const struct k_renderer *k_render_get(void)
{
    return g_renderer;
}
