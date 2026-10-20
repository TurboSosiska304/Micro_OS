#include "port_esp32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"

/*
 * Configuration of the kernel tasks on top of FreeRTOS:
 *   - the base priority keeps kernel tasks above the background tasks of
 *     ESP-IDF (which normally live at priority 1);
 *   - the stack size is a compromise between the limited internal SRAM and
 *     the needs of the drivers.
 */
#define TASK_FREERTOS_PRIO_BASE 9
#define TASK_STACK_SIZE 4096

/*
 * A recursive mutex is used instead of portENTER_CRITICAL(): the FreeRTOS
 * spinlock is not reentrant, while the kernel core is allowed to call a
 * locked helper from inside a locked section.
 */
static SemaphoreHandle_t s_lock;

typedef struct {
    TaskHandle_t handle;
    struct task *task;
    k_port_task_entry_fn entry;
} esp_task_entry;

static esp_task_entry s_registry[TASK_MAX_TASKS];

void port_esp32_console_init(void)
{
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const int uart_num = CONFIG_ESP_CONSOLE_UART_NUM;

    /*
     * Without the UART driver the console can only write: the read side of
     * the ROM console returns the end of the file at once. The driver gives
     * the shell a real input.
     */
    uart_vfs_dev_use_driver(uart_num);

    if (uart_driver_install(uart_num, 256, 0, 0, NULL, 0) != ESP_OK) {
        kerror("console: cannot install the UART driver");
        return;
    }

    uart_param_config(uart_num, &config);
}

void port_esp32_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
}

void k_port_write(const char *data, k_size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    /* write() to fd 1 goes to the UART console driver in ESP-IDF. */
    ssize_t written = write(1, data, len);
    (void)written;
}

k_u64_t k_port_uptime_ms(void)
{
    return (k_u64_t)(esp_timer_get_time() / 1000);
}

void k_port_enter_critical(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void k_port_exit_critical(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

void k_port_halt(void)
{
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void k_port_reset(void)
{
    esp_restart();
}

const char *k_port_name(void)
{
    return "esp32";
}

const char *k_port_arch(void)
{
    return "xtensa-esp32";
}

/* --- console --- */

int k_port_console_readline(char *buf, k_size_t size)
{
    size_t len;

    if (buf == NULL || size == 0) {
        return -1;
    }

    if (fgets(buf, (int)size, stdin) == NULL) {
        return -1;
    }

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    return (int)len;
}

/* --- memory --- */

k_size_t k_port_heap_free(void)
{
    return (k_size_t)esp_get_free_heap_size();
}

k_size_t k_port_heap_total(void)
{
    return (k_size_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

k_size_t k_port_psram_free(void)
{
    return (k_size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

/* --- tasks (FreeRTOS) --- */

k_size_t k_port_task_stack_size(void)
{
    return TASK_STACK_SIZE;
}

static void esp_task_thread(void *arg)
{
    esp_task_entry *entry = (esp_task_entry *)arg;

    /* Wait until the kernel puts the task into the run queue. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    entry->entry(entry->task);
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg)
{
    esp_task_entry *slot = NULL;
    size_t i;
    BaseType_t rc;

    K_UNUSED(arg);

    for (i = 0; i < K_ARRAY_SIZE(s_registry); i++) {
        if (s_registry[i].handle == NULL) {
            slot = &s_registry[i];
            break;
        }
    }

    if (slot == NULL) {
        return -1;
    }

    slot->task = t;
    slot->entry = entry;

    rc = xTaskCreate(esp_task_thread, "ktask", TASK_STACK_SIZE / sizeof(StackType_t), slot,
                     TASK_FREERTOS_PRIO_BASE + t->prio, &slot->handle);

    if (rc != pdPASS) {
        slot->handle = NULL;
        slot->task = NULL;
        return -1;
    }

    t->ctx = slot;
    t->stack = NULL;

    return 0;
}

void k_port_task_start(struct task *t)
{
    esp_task_entry *slot;

    if (t == NULL) {
        return;
    }

    slot = (esp_task_entry *)t->ctx;
    if (slot == NULL || slot->handle == NULL) {
        return;
    }

    xTaskNotifyGive(slot->handle);
}

void k_port_task_yield(void)
{
    taskYIELD();
}

void k_port_task_sleep_ms(k_u32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void k_port_task_exit(int code)
{
    struct task *self = k_port_task_current();

    K_UNUSED(code);

    if (self != NULL) {
        esp_task_entry *slot = (esp_task_entry *)self->ctx;
        if (slot != NULL) {
            slot->handle = NULL;
            slot->task = NULL;
        }
    }

    vTaskDelete(NULL);
}

struct task *k_port_task_current(void)
{
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    size_t i;

    for (i = 0; i < K_ARRAY_SIZE(s_registry); i++) {
        if (s_registry[i].handle != NULL && s_registry[i].handle == handle) {
            return s_registry[i].task;
        }
    }

    return NULL;
}

/* --- time base --- */

static void esp_tick_timer(void *arg)
{
    K_UNUSED(arg);
    k_sched_tick();
}

esp_err_t port_esp32_start_tick(uint32_t period_ms)
{
    const esp_timer_create_args_t args = {
        .callback = esp_tick_timer,
        .name = "microos_tick",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t timer;
    esp_err_t rc;

    if (period_ms == 0) {
        period_ms = 1;
    }

    rc = esp_timer_create(&timer, &args);
    if (rc != ESP_OK) {
        return rc;
    }

    return esp_timer_start_periodic(timer, period_ms * 1000);
}
