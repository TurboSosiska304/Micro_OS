/*
 * Micro OS — microphone module (SPM1423).
 *
 * The MEMS microphone needs an I2S channel and a DMA setup, which belongs to
 * the audio phase of the project. The module is registered without a check so
 * that the doctor reports it as skipped instead of silently ignoring it.
 */
#include "drivers/internal.h"

#include "kernel/kmod.h"

static const struct kmod s_mic_module = {
    .name = "mic",
    .desc = "SPM1423 MEMS microphone (I2S)",
    .init = NULL,
    .check = NULL,
    .detail = NULL,
};

int drv_mic_register(void)
{
    return kmod_register(&s_mic_module);
}
