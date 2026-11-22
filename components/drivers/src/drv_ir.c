/*
 * Micro OS — the infrared pin of the StickC Plus2 (G19).
 *
 * The emitter is a diode on the same pad as the red LED, so every frame is a
 * conversation about who owns the pin: the LED is suspended, the RMT
 * peripheral takes the pad, the carrier of 38 kHz carries the pulses of
 * ir_nec.c, and the pad goes back to the light of the LED afterwards. The
 * command that runs the show is `ir`, and the doctor's module of the same
 * name proves the peripheral and the frame without firing a diode at the
 * boot of a board that may be pointed at a television.
 *
 * A demodulating receiver is not on the board: the Plus2 carries the emitter
 * only. `ir rx` waits on G26, the free pin, for an element a person wired
 * there, and says so when the line stays idle. That is the honest half of the
 * feature - the transmission is the part the board can do by itself.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"
#include "drivers/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/kmod.h"
#include "kernel/lock.h"
#include "kshell/kshell.h"

/*
 * One tick of the RMT is one microsecond: every duration of a frame is
 * written in the units of the standard, and the longest one (9 ms) fits the
 * fifteen bits of a symbol.
 */
#define IR_RESOLUTION_HZ 1000000u
#define IR_CARRIER_HZ 38000u

/* The frame of the doctor: the address and the command of a remote nobody
 * owns. It is built and decoded, never sent. */
#define IR_CHECK_ADDRESS 0x04
#define IR_CHECK_COMMAND 0x08

/* The longest wait for one frame of the receiver. */
#define IR_RX_TIMEOUT_MS 5000u

/* A level that lasts longer than this ends a frame: the longest pulse of a
 * NEC command is the 9 ms header mark, and the silence after the stop mark
 * is longer than this. */
#define IR_RX_IDLE_US 15000u

/* The place of the received symbols: worst case one symbol per pulse, and a
 * little room for the pairs an idle line adds. */
#define IR_RX_SYMBOLS 64

static k_mutex_t s_tx_lock = K_MUTEX_INITIALIZER;
static k_mutex_t s_rx_lock = K_MUTEX_INITIALIZER;
static bool s_rmt_ok;
static volatile bool s_tx_owns_pin;

/* The last frame the receiver assembled, for the callback of the ISR: the
 * count is written by the interrupt and read by the waiting task. */
static rmt_symbol_word_t s_rx_symbols[IR_RX_SYMBOLS];
static volatile size_t s_rx_count;
static TaskHandle_t s_rx_waiting;

static int drv_ir_init(void)
{
    rmt_tx_channel_config_t config = {
        .gpio_num = PIN_IR,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    rmt_channel_handle_t probe = NULL;

    /* The channel on G19 proves the peripheral and the pad without firing a
     * diode: the driver of the RMT refuses a channel without a GPIO, so the
     * probe takes the real one, and the light gets the pad back at once. */
    s_rmt_ok = rmt_new_tx_channel(&config, &probe) == ESP_OK;

    if (s_rmt_ok) {
        (void)rmt_del_channel(probe);
    }

    (void)drv_led_resume();

    return s_rmt_ok ? 0 : -1;
}

bool drv_ir_owns_pin(void)
{
    return s_tx_owns_pin;
}

/* The pulses as the symbols of the RMT: level high for a mark, low for a
 * space, in microseconds of the channel. */
static void ir_to_symbols(const struct ir_pulse *pulses, unsigned count, rmt_symbol_word_t *symbols)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        symbols[i].duration0 = pulses[i].mark_us;
        symbols[i].level0 = 1;
        symbols[i].duration1 = pulses[i].space_us;
        symbols[i].level1 = 0;
    }
}

int drv_ir_send_nec(k_u8_t address, k_u8_t command)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    rmt_symbol_word_t symbols[IR_NEC_PULSES_MAX];
    rmt_tx_channel_config_t channel = {
        .gpio_num = PIN_IR,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    rmt_carrier_config_t carrier = {
        .frequency_hz = IR_CARRIER_HZ,
        .duty_cycle = 0.33f,
    };
    rmt_transmit_config_t transmit = {
        .loop_count = 0,
    };
    /* The encoder of the copy has no options: the symbols are the payload. */
    rmt_copy_encoder_config_t copy = {};
    rmt_channel_handle_t tx = NULL;
    rmt_encoder_handle_t encoder = NULL;
    unsigned count;
    int rc = 0;

    count = ir_nec_build(address, command, pulses, IR_NEC_PULSES_MAX);
    if (count == 0) {
        return -1;
    }

    if (k_mutex_lock(&s_tx_lock) != 0) {
        return -1;
    }

    /*
     * The light goes dark before the pad is re-routed: a fade that was on
     * its way up must not leave the LED glowing while the emitter sends.
     */
    s_tx_owns_pin = true;
    (void)drv_led_suspend();

    ir_to_symbols(pulses, count, symbols);

    if (rmt_new_tx_channel(&channel, &tx) != ESP_OK) {
        rc = -2;
        tx = NULL;
        goto done;
    }

    if (rmt_apply_carrier(tx, &carrier) != ESP_OK || rmt_enable(tx) != ESP_OK) {
        rc = -2;
        goto done;
    }

    if (rmt_new_copy_encoder(&copy, &encoder) != ESP_OK) {
        rc = -2;
        goto done;
    }

    if (rmt_transmit(tx, encoder, symbols, (size_t)count * sizeof(symbols[0]), &transmit) !=
        ESP_OK) {
        rc = -3;
        goto done;
    }

    if (rmt_tx_wait_all_done(tx, 200) != ESP_OK) {
        rc = -3;
    }

done:
    if (encoder != NULL) {
        (void)rmt_del_encoder(encoder);
    }
    if (tx != NULL) {
        (void)rmt_disable(tx);
        (void)rmt_del_channel(tx);
    }

    /* The pad goes back to the light of the LED, dark. */
    (void)gpio_reset_pin(PIN_IR);
    (void)drv_led_resume();

    s_tx_owns_pin = false;
    k_mutex_unlock(&s_tx_lock);

    return rc;
}

/* --- the receiver --- */

/*
 * The pulses of the received symbols. A symbol holds two levels and the
 * levels of two symbols can be halves of one pulse, so the timeline is walked
 * once and every pair of "high then low" becomes a pulse.
 */
static unsigned ir_from_symbols(const rmt_symbol_word_t *symbols, size_t count,
                                struct ir_pulse *pulses, unsigned max)
{
    unsigned made = 0;
    unsigned long mark = 0;
    bool in_mark = false;
    size_t i;

    for (i = 0; i < count; i++) {
        const rmt_symbol_word_t *symbol = &symbols[i];
        unsigned levels[2] = {symbol->level0, symbol->level1};
        unsigned durations[2] = {symbol->duration0, symbol->duration1};
        int part;

        for (part = 0; part < 2; part++) {
            if (durations[part] == 0) {
                continue;
            }

            if (levels[part] != 0) {
                mark += durations[part];
                in_mark = true;
                continue;
            }

            if (!in_mark) {
                /* A space before the first mark is the line resting. */
                continue;
            }

            if (made >= max) {
                return made;
            }

            pulses[made].mark_us = (uint16_t)K_MIN(mark, 0xFFFFUL);
            pulses[made].space_us = (uint16_t)K_MIN((unsigned long)durations[part], 0xFFFFUL);
            made++;
            mark = 0;
            in_mark = false;
        }
    }

    /* A frame that ends on a mark (the stop bit of some remotes) still has
     * its last pulse. */
    if (in_mark && made < max) {
        pulses[made].mark_us = (uint16_t)K_MIN(mark, 0xFFFFUL);
        pulses[made].space_us = 0;
        made++;
    }

    return made;
}

static bool ir_rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event,
                       void *user)
{
    BaseType_t woken = pdFALSE;

    K_UNUSED(channel);
    K_UNUSED(user);

    s_rx_count = event->num_symbols;
    if (event->num_symbols > IR_RX_SYMBOLS) {
        s_rx_count = IR_RX_SYMBOLS;
    } else if (event->received_symbols != s_rx_symbols) {
        memcpy(s_rx_symbols, event->received_symbols, event->num_symbols * sizeof(s_rx_symbols[0]));
    }

    if (s_rx_waiting != NULL) {
        vTaskNotifyGiveFromISR(s_rx_waiting, &woken);
    }

    return woken == pdTRUE;
}

int drv_ir_receive_nec(struct ir_nec_frame *frame, k_u32_t timeout_ms)
{
    struct ir_pulse pulses[IR_RX_SYMBOLS];
    rmt_rx_channel_config_t channel = {
        .gpio_num = PIN_IR_RX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_SYMBOLS,
        .flags.invert_in = 1,
    };
    rmt_receive_config_t receive = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = IR_RX_IDLE_US * 1000u,
    };
    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = ir_rx_done,
    };
    rmt_channel_handle_t rx = NULL;
    unsigned made;
    int rc = 1;

    if (frame == NULL) {
        return -1;
    }

    timeout_ms = K_MIN(timeout_ms, 60000u);

    if (k_mutex_lock(&s_rx_lock) != 0) {
        return -2;
    }

    /* The line of a receiver that is not there floats; the pull-up keeps it
     * at the idle level and the RMT sees no frame. */
    (void)gpio_set_pull_mode(PIN_IR_RX, GPIO_PULLUP_ONLY);

    if (rmt_new_rx_channel(&channel, &rx) != ESP_OK) {
        k_mutex_unlock(&s_rx_lock);
        return -2;
    }

    s_rx_count = 0;
    s_rx_waiting = xTaskGetCurrentTaskHandle();

    if (rmt_rx_register_event_callbacks(rx, &callbacks, NULL) != ESP_OK ||
        rmt_enable(rx) != ESP_OK ||
        rmt_receive(rx, s_rx_symbols, sizeof(s_rx_symbols), &receive) != ESP_OK) {
        rc = -2;
        goto done;
    }

    /* The notification comes from the callback of the RMT when the line has
     * been idle for the length of a frame; the wait is bounded. */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        rc = 1;
        goto done;
    }

    made = ir_from_symbols(s_rx_symbols, s_rx_count, pulses, IR_RX_SYMBOLS);

    if (ir_nec_decode(pulses, made, frame)) {
        rc = 0;
    } else if (made > 0) {
        rc = -3;
    } else {
        rc = 1;
    }

done:
    s_rx_waiting = NULL;
    (void)rmt_disable(rx);
    (void)rmt_del_channel(rx);
    (void)gpio_reset_pin(PIN_IR_RX);
    k_mutex_unlock(&s_rx_lock);

    return rc;
}

/* --- the doctor and the shell --- */

static const char *drv_ir_detail(void)
{
    return "NEC tx on G19, shared with the led; no rx";
}

static int drv_ir_check(void)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    struct ir_nec_frame frame;
    unsigned count;

    if (!s_rmt_ok) {
        return -1;
    }

    /* The frame the emitter would send is built and read back: the timing
     * of the protocol and the arithmetic of the driver, without a diode
     * glowing at a television in the room. */
    count = ir_nec_build(IR_CHECK_ADDRESS, IR_CHECK_COMMAND, pulses, IR_NEC_PULSES_MAX);

    if (count != IR_NEC_PULSES_MAX) {
        return -1;
    }

    if (!ir_nec_decode(pulses, count, &frame)) {
        return -1;
    }

    if (frame.address != IR_CHECK_ADDRESS || frame.command != IR_CHECK_COMMAND) {
        return -1;
    }

    return 0;
}

static const struct kmod s_ir_module = {
    .name = "ir",
    .desc = "IR emitter on G19, receiver on G26",
    .init = drv_ir_init,
    .check = drv_ir_check,
    .detail = drv_ir_detail,
};

/* The length of a frame of the builder, in whole milliseconds. */
static unsigned ir_frame_ms(const struct ir_pulse *pulses, unsigned count)
{
    unsigned long us = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        us += pulses[i].mark_us + pulses[i].space_us;
    }

    return (unsigned)((us + 500) / 1000);
}

/* A byte of a frame: decimal, or hexadecimal with the 0x of the usual
 * writing. The base is chosen by the prefix and not left to strtoul: base 0
 * reads "04" as an octal and stops at the 8 of "08". */
static bool ir_parse_byte(const char *text, k_u8_t *out)
{
    char *end = NULL;
    int base = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;
    unsigned long value = strtoul(text, &end, base);

    if (text[0] == '\0' || end == NULL || *end != '\0' || value > 0xFF) {
        return false;
    }

    *out = (k_u8_t)value;

    return true;
}

static bool ir_parse_fields(const char *address, const char *command, k_u8_t *out_address,
                            k_u8_t *out_command)
{
    return ir_parse_byte(address, out_address) && ir_parse_byte(command, out_command);
}

static void ir_usage(void)
{
    kshell_printf("usage: ir | ir frame <address> <command> | ir send <address> <command>\n");
    kshell_printf("       ir rx [ms]\n");
}

/* `ir`: who owns the pin, and what the emitter would send. */
static int ir_show(void)
{
    kshell_printf("ir: NEC on G19, shared with the led (%s)\n",
                  drv_ir_owns_pin() ? "the emitter has it" : "the led has it");
    kshell_printf("ir: no receiver on the board; `ir rx` waits on G26\n");

    return KSHELL_OK;
}

static int cmd_ir(int argc, char **argv)
{
    struct ir_nec_frame frame;
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    k_u8_t address = 0;
    k_u8_t command = 0;
    unsigned count;
    int rc;

    if (argc == 1) {
        return ir_show();
    }

    if (strcmp(argv[1], "rx") == 0) {
        unsigned long timeout = IR_RX_TIMEOUT_MS;

        if (argc > 2) {
            char *end = NULL;

            timeout = strtoul(argv[2], &end, 10);
            if (argv[2][0] == '\0' || end == NULL || *end != '\0' || timeout == 0) {
                kshell_printf("ir rx: no such number of milliseconds: %s\n", argv[2]);
                return KSHELL_ERROR;
            }
        }

        rc = drv_ir_receive_nec(&frame, (k_u32_t)timeout);

        if (rc == 0) {
            kshell_printf("ir: nec %02x:%02x\n", frame.address, frame.command);
            return KSHELL_OK;
        }
        if (rc == 1) {
            kshell_printf("ir: no frame in %lu ms (there is no receiver on the board)\n", timeout);
            return KSHELL_OK;
        }
        if (rc == -3) {
            kshell_printf("ir: something was received and it is not a NEC frame\n");
            return KSHELL_ERROR;
        }

        kshell_printf("ir: the receiver cannot be started\n");
        return KSHELL_ERROR;
    }

    if (argc != 4 || (strcmp(argv[1], "send") != 0 && strcmp(argv[1], "frame") != 0)) {
        ir_usage();
        return KSHELL_USAGE;
    }

    if (!ir_parse_fields(argv[2], argv[3], &address, &command)) {
        kshell_printf("ir: the address and the command are two numbers 0..255\n");
        return KSHELL_ERROR;
    }

    count = ir_nec_build(address, command, pulses, IR_NEC_PULSES_MAX);
    if (count == 0) {
        kshell_printf("ir: the frame cannot be built\n");
        return KSHELL_ERROR;
    }

    if (strcmp(argv[1], "frame") == 0) {
        kshell_printf("ir: nec %02x:%02x, %u pulses, %u ms (not sent)\n", address, command, count,
                      ir_frame_ms(pulses, count));
        return KSHELL_OK;
    }

    rc = drv_ir_send_nec(address, command);

    if (rc != 0) {
        if (rc == -2) {
            kshell_printf("ir: the rmt peripheral cannot take G19\n");
        } else {
            kshell_printf("ir: the frame did not leave the emitter\n");
        }
        return KSHELL_ERROR;
    }

    kshell_printf("ir: sent nec %02x:%02x, %u pulses, %u ms\n", address, command, count,
                  ir_frame_ms(pulses, count));

    return KSHELL_OK;
}

int drv_ir_register(void)
{
    int rc = kmod_register(&s_ir_module);

    kshell_register("ir", "the infrared: ir, ir frame|send <a> <c>, ir rx [ms]", cmd_ir);

    return rc;
}
