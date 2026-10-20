/*
 * Micro OS — memory modules: PSRAM and internal flash.
 */
#include "drivers/internal.h"

#include <stdio.h>

#include "esp_flash.h"
#include "esp_psram.h"
#include "kernel/kmod.h"

static size_t s_psram_size;
static size_t s_flash_size;

static int drv_psram_init(void)
{
    s_psram_size = esp_psram_get_size();
    return s_psram_size > 0 ? 0 : -1;
}

static int drv_psram_check(void)
{
    return s_psram_size > 0 ? 0 : -1;
}

static const char *drv_psram_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(s_psram_size / 1024));
    return buf;
}

static int drv_flash_init(void)
{
    esp_flash_get_size(NULL, &s_flash_size);
    return s_flash_size > 0 ? 0 : -1;
}

static int drv_flash_check(void)
{
    return s_flash_size > 0 ? 0 : -1;
}

static const char *drv_flash_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(s_flash_size / (1024 * 1024)));
    return buf;
}

static const struct kmod s_psram_module = {
    .name = "psram",
    .desc = "external PSRAM",
    .init = drv_psram_init,
    .check = drv_psram_check,
    .detail = drv_psram_detail,
};

static const struct kmod s_flash_module = {
    .name = "flash",
    .desc = "internal SPI flash",
    .init = drv_flash_init,
    .check = drv_flash_check,
    .detail = drv_flash_detail,
};

int drv_memory_register(void)
{
    int rc = kmod_register(&s_psram_module);

    if (kmod_register(&s_flash_module) != 0) {
        rc = -1;
    }

    return rc;
}
