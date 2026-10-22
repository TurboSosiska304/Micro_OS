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
#define TASK_STACK_SIZE 8192

/*
 * The critical section of the kernel. The FreeRTOS spinlock of the chip
 * is recursive on one core (it counts the nesting), it is taken with the
 * interrupts off, and it works from a task and from an interrupt alike -
 * so it is the section the kernel says it is. A mutex was not: it let an
 * interrupt in the middle of a "critical" section, and it was a silent
 * no-op when it was not created yet.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    TaskHandle_t handle;
    struct task *task;
    k_port_task_entry_fn entry;
    /*
     * The slot belongs to a task from the moment it is chosen until the
     * thread of that task has deleted itself. The scan of the free slots
     * looks at this flag, not at handle: a slot whose handle was cleared
     * is free before its task pointer is, and a task created in that
     * moment would be given a slot that another thread is still erasing.
     */
    bool taken;
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
    /* The critical section needs no preparation: the spinlock is static. */
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
    portENTER_CRITICAL(&s_lock);
}

void k_port_exit_critical(void)
{
    portEXIT_CRITICAL(&s_lock);
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

int k_port_console_getchar(void)
{
    uint8_t c = 0;
    int rc = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY);

    if (rc != 1) {
        return -1;
    }

    return (int)c;
}

/* --- memory --- */

k_size_t k_port_heap_free(void)
{
    /* The internal heap only, the PSRAM is reported separately. */
    return (k_size_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

k_size_t k_port_heap_total(void)
{
    return (k_size_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

k_size_t k_port_psram_free(void)
{
    return (k_size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

k_size_t k_port_psram_total(void)
{
    return (k_size_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

void *k_port_alloc(k_size_t size, bool psram)
{
    if (size == 0) {
        size = 4096;
    }

    return heap_caps_malloc(size, psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
}

void k_port_free(void *ptr)
{
    heap_caps_free(ptr);
}

k_u64_t k_port_uptime_us(void)
{
    return (k_u64_t)esp_timer_get_time();
}

/* --- tasks (FreeRTOS) --- */

k_size_t k_port_task_stack_size(void)
{
    return TASK_STACK_SIZE;
}

static void esp_task_thread(void *arg)
{
    esp_task_entry *entry = (esp_task_entry *)arg;
    k_port_task_entry_fn fn;
    struct task *task;

    /* Wait until the kernel puts the task into the run queue. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /*
     * The work is done with the values taken here: the slot can be given
     * to another task as soon as this thread ends, and the thread must
     * not read the slot again after that.
     */
    fn = entry->entry;
    task = entry->task;

    fn(task);
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg)
{
    esp_task_entry *slot = NULL;
    size_t i;
    BaseType_t rc;

    K_UNUSED(arg);

    /*
     * A free slot is taken under the critical section: two tasks that
     * create a task at the same time must not be given the same slot.
     */
    k_port_enter_critical();

    for (i = 0; i < K_ARRAY_SIZE(s_registry); i++) {
        if (!s_registry[i].taken) {
            slot = &s_registry[i];
            slot->taken = true;
            slot->task = t;
            slot->entry = entry;
            slot->handle = NULL;
            break;
        }
    }

    k_port_exit_critical();

    if (slot == NULL) {
        return -1;
    }

    rc = xTaskCreate(esp_task_thread, "ktask", TASK_STACK_SIZE / sizeof(StackType_t), slot,
                     TASK_FREERTOS_PRIO_BASE + t->prio, &slot->handle);

    if (rc != pdPASS) {
        k_port_enter_critical();
        slot->handle = NULL;
        slot->task = NULL;
        slot->entry = NULL;
        slot->taken = false;
        k_port_exit_critical();
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
    if (slot == NULL) {
        return;
    }

    k_port_enter_critical();
    {
        TaskHandle_t handle = slot->handle;

        k_port_exit_critical();

        if (handle != NULL) {
            xTaskNotifyGive(handle);
        }
    }
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
            /*
             * The slot is given back in one step, and the thread does not
             * touch it again: a task that is created in the same moment
             * finds a slot that is either taken or free, never half.
             */
            k_port_enter_critical();
            slot->handle = NULL;
            slot->task = NULL;
            slot->entry = NULL;
            slot->taken = false;
            k_port_exit_critical();
        }
    }

    vTaskDelete(NULL);
}

struct task *k_port_task_current(void)
{
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    struct task *found = NULL;
    size_t i;

    k_port_enter_critical();

    for (i = 0; i < K_ARRAY_SIZE(s_registry); i++) {
        if (s_registry[i].handle != NULL && s_registry[i].handle == handle) {
            found = s_registry[i].task;
            break;
        }
    }

    k_port_exit_critical();

    return found;
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

    /*
     * The service of the timer is made by a startup hook, which the
     * linker keeps as soon as something uses the esp_timer API - and the
     * port does (the tick itself), so the hook is always there. Calling
     * the initialisation by hand makes the driver print a red line at
     * every boot of the board: it says that the service is already up,
     * which is exactly the state it has to be in.
     */
    rc = esp_timer_create(&args, &timer);
    if (rc != ESP_OK) {
        kerror("tick: cannot create the timer (rc=%d)", (int)rc);
        return rc;
    }

    rc = esp_timer_start_periodic(timer, period_ms * 1000);
    if (rc != ESP_OK) {
        kerror("tick: cannot start the timer (rc=%d)", (int)rc);
        return rc;
    }

    kinfo("tick: the %u ms time base is running", (unsigned)period_ms);

    return ESP_OK;
}
