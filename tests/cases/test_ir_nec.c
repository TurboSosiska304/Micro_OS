/*
 * Micro OS — the frame of an infrared remote.
 *
 * The builder and the reader are two halves of one contract, so the test is
 * mostly the round trip through every address and command: what leaves the
 * emitter has to come back out of the reader the same. The interesting cases
 * are the ones a real remote makes: the same byte of a different value, the
 * complement that disagrees, a pulse stretched by a slow receiver, and a
 * burst of noise that is not a frame at all.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "drivers/ir_nec.h"

static void test_build(void)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];

    ktest_begin("ir: the frame of a NEC command has the timings of the standard");

    KT_CHECK_INT_EQ(ir_nec_build(0x00, 0x00, pulses, IR_NEC_PULSES_MAX), IR_NEC_PULSES_MAX);

    /* The header, a bit and the stop mark: the three lengths of the frame. */
    KT_CHECK_INT_EQ(pulses[0].mark_us, 9000);
    KT_CHECK_INT_EQ(pulses[0].space_us, 4500);
    KT_CHECK_INT_EQ(pulses[1].mark_us, 560);
    KT_CHECK_INT_EQ(pulses[1].space_us, 560);
    KT_CHECK_INT_EQ(pulses[IR_NEC_PULSES_MAX - 1].mark_us, 560);
    KT_CHECK_INT_EQ(pulses[IR_NEC_PULSES_MAX - 1].space_us, 0);

    /* 0xFF is four bytes of ones with their complements: the address and
     * the command are all ones, the two bytes between them all zeros. The
     * bits sit at the pulses 1..32, least significant bit first. */
    KT_CHECK_INT_EQ(ir_nec_build(0xFF, 0xFF, pulses, IR_NEC_PULSES_MAX), IR_NEC_PULSES_MAX);
    KT_CHECK_INT_EQ(pulses[1].space_us, 1690);  /* address, bit 0 */
    KT_CHECK_INT_EQ(pulses[9].space_us, 560);   /* its complement, bit 0 */
    KT_CHECK_INT_EQ(pulses[17].space_us, 1690); /* command, bit 0 */
    KT_CHECK_INT_EQ(pulses[25].space_us, 560);  /* its complement, bit 0 */

    /* 0x01 with a command of 0: the address has its bit 0 and the complement
     * of the address is 0xFE, which has it clear. */
    KT_CHECK_INT_EQ(ir_nec_build(0x01, 0x00, pulses, IR_NEC_PULSES_MAX), IR_NEC_PULSES_MAX);
    KT_CHECK_INT_EQ(pulses[1].space_us, 1690);  /* address bit 0 */
    KT_CHECK_INT_EQ(pulses[2].space_us, 560);   /* address bit 1 */
    KT_CHECK_INT_EQ(pulses[9].space_us, 560);   /* complement bit 0 */
    KT_CHECK_INT_EQ(pulses[10].space_us, 1690); /* complement bit 1 */

    /* A place too small does not hold half a frame. */
    KT_CHECK_INT_EQ(ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX - 1), 0);
    KT_CHECK_INT_EQ(ir_nec_build(0x04, 0x08, NULL, IR_NEC_PULSES_MAX), 0);
}

static void test_round_trip(void)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    struct ir_nec_frame frame;
    unsigned address;
    unsigned command;
    unsigned checked = 0;

    ktest_begin("ir: every address and command comes back out of the frame");

    /* All 256 addresses against all 256 commands is 65536 frames; the test
     * walks the whole square, which is a second on a laptop and the cheapest
     * proof that no bit is in the wrong place. */
    for (address = 0; address < 256; address++) {
        for (command = 0; command < 256; command++) {
            unsigned count =
                ir_nec_build((uint8_t)address, (uint8_t)command, pulses, IR_NEC_PULSES_MAX);

            if (count != IR_NEC_PULSES_MAX || !ir_nec_decode(pulses, count, &frame)) {
                ktest_fail(__FILE__, __LINE__, "the frame %02x:%02x does not decode", address,
                           command);
                return;
            }

            if (frame.address != (uint8_t)address || frame.command != (uint8_t)command) {
                ktest_fail(__FILE__, __LINE__, "%02x:%02x came back as %02x:%02x", address, command,
                           frame.address, frame.command);
                return;
            }

            checked++;
        }
    }

    KT_CHECK(checked == 65536u);
}

static void test_tolerance(void)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    struct ir_nec_frame frame;
    unsigned i;

    ktest_begin("ir: the reader takes a receiver that is a quarter off");

    (void)ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX);

    /* A slow receiver stretches every pulse by twenty percent. */
    for (i = 0; i < IR_NEC_PULSES_MAX; i++) {
        pulses[i].mark_us = (uint16_t)(pulses[i].mark_us + pulses[i].mark_us / 5);
        pulses[i].space_us = (uint16_t)(pulses[i].space_us + pulses[i].space_us / 5);
    }

    KT_CHECK(ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));
    KT_CHECK_INT_EQ(frame.address, 0x04);
    KT_CHECK_INT_EQ(frame.command, 0x08);

    /* Beyond a quarter the 0 and the 1 of the protocol would meet, and the
     * reader refuses instead of guessing. */
    (void)ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX);
    pulses[1].space_us = 1000; /* between the windows of a 0 and of a 1 */

    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));
}

static void test_refusals(void)
{
    struct ir_pulse pulses[IR_NEC_PULSES_MAX];
    struct ir_nec_frame frame;

    ktest_begin("ir: what is not a NEC frame is not read as one");

    (void)ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX);

    KT_CHECK(!ir_nec_decode(NULL, IR_NEC_PULSES_MAX, &frame));
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, NULL));

    /* A truncated frame: the header alone, or half of the bits. */
    KT_CHECK(!ir_nec_decode(pulses, 1, &frame));
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX - 1, &frame));

    /* A header of another protocol: 2.4 ms of mark is not NEC. */
    pulses[0].mark_us = 2400;
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));

    /* A byte whose complement disagrees: noise with the rhythm of a frame.
     * The bit 2 of the address is a 1 of 0x04, and the complement keeps its
     * own copy of it: turning one into a 0 without the other is exactly the
     * damage the check exists for. */
    (void)ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX);
    pulses[1 + 2].space_us = 560;
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));

    /* No stop mark. */
    (void)ir_nec_build(0x04, 0x08, pulses, IR_NEC_PULSES_MAX);
    pulses[IR_NEC_PULSES_MAX - 1].mark_us = 3000;
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));

    /* A buffer of silence has no header, so it is not a frame of zeros. */
    memset(pulses, 0, sizeof(pulses));
    KT_CHECK(!ir_nec_decode(pulses, IR_NEC_PULSES_MAX, &frame));
}

void test_ir_nec(void)
{
    test_build();
    test_round_trip();
    test_tolerance();
    test_refusals();
}
