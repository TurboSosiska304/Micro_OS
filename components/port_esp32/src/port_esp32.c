#include "port_esp32.h"

#include <unistd.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/port.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void port_esp32_console_init(void) {
    /* UART0 is already configured by the ROM bootloader at this point. */
}

void k_port_write(const char *data, k_size_t len) {
    if (data == NULL || len == 0) {
        return;
    }

    /* write() to fd 1 goes to the UART console driver in ESP-IDF. */
    ssize_t written = write(1, data, len);
    (void)written;
}

k_u64_t k_port_uptime_ms(void) {
    return (k_u64_t)(esp_timer_get_time() / 1000);
}

void k_port_enter_critical(void) {
    portENTER_CRITICAL(&s_mux);
}

void k_port_exit_critical(void) {
    portEXIT_CRITICAL(&s_mux);
}

void k_port_halt(void) {
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void k_port_reset(void) {
    esp_restart();
}

const char *k_port_name(void) {
    return "esp32";
}

const char *k_port_arch(void) {
    return "xtensa-esp32";
}
