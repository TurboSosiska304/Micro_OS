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
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "drivers/power.h"

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
    port_esp32_blockdev_init();
#ifdef CONFIG_MICRO_OS_WIFI
    port_esp32_wifi_init();
#endif
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

/*
 * The panel of the board is the console and no program owns it (ADR 0028
 * leaves the board out of the interface of 1.4), so a panic has no screen to
 * take back. The call is here because the contract has it on every machine.
 */
void k_port_console_panic(void) {}

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

/*
 * The recursive mutex of FreeRTOS: it counts how many times the task that
 * owns it has taken it, and it sleeps the others instead of spinning. The
 * structure stays opaque to the kernel, which only sees a handle.
 */
struct k_port_mutex {
    SemaphoreHandle_t handle;
};

k_port_mutex_t *k_port_mutex_create(void)
{
    k_port_mutex_t *mutex = calloc(1, sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }

    mutex->handle = xSemaphoreCreateRecursiveMutex();
    if (mutex->handle == NULL) {
        free(mutex);
        return NULL;
    }

    return mutex;
}

int k_port_mutex_destroy(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    if (mutex->handle != NULL) {
        vSemaphoreDelete(mutex->handle);
    }

    free(mutex);

    return 0;
}

int k_port_mutex_lock(k_port_mutex_t *mutex)
{
    if (mutex == NULL || mutex->handle == NULL) {
        return -1;
    }

    return xSemaphoreTakeRecursive(mutex->handle, portMAX_DELAY) == pdTRUE ? 0 : -1;
}

int k_port_mutex_unlock(k_port_mutex_t *mutex)
{
    if (mutex == NULL || mutex->handle == NULL) {
        return -1;
    }

    return xSemaphoreGiveRecursive(mutex->handle) == pdTRUE ? 0 : -1;
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

/*
 * The last step of the clean end (ADR 0050) on a board with a power latch.
 *
 * The board has no PMIC and no S5: "off" is the deep sleep of the chip with
 * HOLD kept high and BUTTON C as the wake source, so a person who presses
 * the button gets the board back and the volume it left clean. A wake from
 * that sleep is a restart of the chip - the RAM does not survive it - and
 * the boot says "power: woke from ext1" (src/drv_power.c). A sleep that
 * cannot arm the button would be a board with no way back: the driver drops
 * the latch instead, which is the old `power off` of the board.
 *
 * The reboot is the chip's own, and the halt is the idle loop of the
 * kernel on FreeRTOS: the processor stays in the task of the shutdown and
 * the board keeps its power for a person who reads the sentence.
 */
void k_port_power(enum k_power_action action)
{
    switch (action) {
    case K_POWER_HALT:
        kinfo("esp32: the processor stops, the power stays on");
        k_port_halt();
        break;

    case K_POWER_OFF:
        kinfo("esp32: the board goes to deep sleep, button C wakes it");
        if (drv_power_deep_off() != 0) {
            kwarn("esp32: the wake source did not arm, the latch is dropped");
            (void)drv_power_off();
        }
        /* drv_power_off() answers with the cable in; the machine stays
         * awake then, and there is nothing else this port can do. */
        k_port_halt();
        break;

    case K_POWER_REBOOT:
        kinfo("esp32: the board restarts");
        esp_restart();
        break;
    }
}

const char *k_port_name(void)
{
    return "esp32";
}

const char *k_port_arch(void)
{
    return "xtensa-esp32";
}

/*
 * The contract this port was written for. The assert turns a header that
 * grew (or changed its mind) into a build error instead of a call with the
 * wrong meaning, the way vermagic does for a module of Linux.
 */
#define PORT_API_MAJOR 1u
#define PORT_API_MINOR 13u

_Static_assert(PORT_API_MAJOR == KERNEL_PORT_API_VERSION_MAJOR,
               "port_esp32 does not speak the contract of this kernel");

unsigned k_port_api_major(void)
{
    return PORT_API_MAJOR;
}

unsigned k_port_api_minor(void)
{
    return PORT_API_MINOR;
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

int k_port_console_poll(void)
{
    uint8_t c = 0;

    /* A zero wait: the driver answers at once, with 0 when nothing waits. */
    if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, 0) != 1) {
        return -1;
    }

    return (int)c;
}

/*
 * The size of the console of the board: the panel is 80 columns of 25 rows
 * (docs/ports/esp32/hardware.md), and a program of the board has no window to
 * draw in that would be another number.
 */
void k_port_console_size(k_u16_t *rows, k_u16_t *columns)
{
    if (rows != NULL) {
        *rows = 25u;
    }
    if (columns != NULL) {
        *columns = 80u;
    }
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

    /*
     * Memory the kernel writes to has to be the kind the data bus can
     * reach: MALLOC_CAP_INTERNAL alone also matches the exec-only IRAM
     * region of the ESP32, where a store faults. MALLOC_CAP_8BIT keeps
     * the allocation on byte-writable memory (DRAM or PSRAM).
     */
    return heap_caps_malloc(size,
                            psram ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void k_port_free(void *ptr)
{
    heap_caps_free(ptr);
}

k_u64_t k_port_uptime_us(void)
{
    return (k_u64_t)esp_timer_get_time();
}

/*
 * The memory a program can map. The board has no page tables for a program:
 * a program is a task of the kernel and shares its memory, so every call of
 * kernel/port/mm.h answers "no" and `mmap` is ENOSYS there.
 */
bool k_port_mm_supported(void)
{
    return false;
}

k_u64_t k_port_mm_base(void)
{
    return 0;
}

k_u64_t k_port_mm_limit(void)
{
    return 0;
}

k_u64_t k_port_mm_top(void)
{
    return 0;
}

k_u64_t k_port_mm_current(void)
{
    return 0;
}

void *k_port_mm_page(k_u64_t space, k_u64_t vaddr, k_u32_t prot)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(prot);
    return NULL;
}

void *k_port_mm_writable(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void *k_port_mm_private(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

bool k_port_mm_present(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return false;
}

void *k_port_mm_phys(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void k_port_mm_unmap(k_u64_t space, k_u64_t vaddr, k_size_t len)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(len);
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
        kerror("esp32: xTaskCreate failed: free %u", (unsigned)k_port_heap_free());
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
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    size_t i;

    K_UNUSED(code);

    /*
     * The slot is found by the handle of the task that is leaving and not
     * through the record of the kernel: the parent may reap a zombie as soon
     * as it is published, which zeroes that record, and the registry slot
     * would then be lost for the life of the board - after thirty programs
     * no task could be created. The handle is the port's own name of the
     * task and the one thing a parent cannot take away.
     */
    k_port_enter_critical();

    for (i = 0; i < K_ARRAY_SIZE(s_registry); i++) {
        if (s_registry[i].taken && s_registry[i].handle == self) {
            s_registry[i].handle = NULL;
            s_registry[i].task = NULL;
            s_registry[i].entry = NULL;
            s_registry[i].taken = false;
            break;
        }
    }

    k_port_exit_critical();

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

/*
 * A task of the board is a FreeRTOS task, not a program of ring 3: the kernel
 * runs the handler of a signal in the context of the task itself, and there
 * is no frame to restore. A program of the board (ADR 0018) is a task of the
 * kernel as well, so its handler is an ordinary function of the program's
 * text called on the stack of its task; the door of the port is what the
 * handler calls back. The handler's answer takes the place of the interrupted
 * call only on a machine with a frame of its own (x86-64), so the answer here
 * is the one the call had.
 */
int k_port_task_signal_enter(struct task *t, int sig, void (*handler)(int), k_ssize_t result,
                             bool from_syscall)
{
    K_UNUSED(t);
    K_UNUSED(sig);
    K_UNUSED(handler);
    K_UNUSED(result);
    K_UNUSED(from_syscall);
    return 0;
}

int k_port_task_signal_leave(struct task *t, k_ssize_t *result)
{
    K_UNUSED(t);
    K_UNUSED(result);
    return KERR_NOSYS;
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
