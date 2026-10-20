#include "port_esp32.h"

#include <stdlib.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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

void port_esp32_console_init(void) {
    /* UART0 is already configured by the ROM bootloader at this point. */
}

void port_esp32_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
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
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void k_port_exit_critical(void) {
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
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

/* --- tasks (FreeRTOS) --- */

k_size_t k_port_task_stack_size(void) {
    return TASK_STACK_SIZE;
}

static void esp_task_thread(void *arg) {
    esp_task_entry *entry = (esp_task_entry *)arg;

    /* Wait until the kernel puts the task into the run queue. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    entry->entry(entry->task);
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg) {
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

void k_port_task_start(struct task *t) {
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

void k_port_task_yield(void) {
    taskYIELD();
}

void k_port_task_sleep_ms(k_u32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void k_port_task_exit(int code) {
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

struct task *k_port_task_current(void) {
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

static void esp_tick_timer(void *arg) {
    K_UNUSED(arg);
    k_sched_tick();
}

esp_err_t port_esp32_start_tick(uint32_t period_ms) {
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
