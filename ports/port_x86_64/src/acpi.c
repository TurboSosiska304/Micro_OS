/*
 * Micro OS — the soft off and the reset of a PC (ACPI, ADR 0050).
 *
 * A machine of the PC family does not have a port that removes its power.
 * It has the fixed ACPI description table: the FADT names the control
 * block of PM1a (and of PM1b, on a machine with two), and the write of the
 * sleep type of S5 with the sleep enable bit is the request "switch
 * yourself off". The processor stops inside the write, the chipset drops
 * the rails and the machine is gone.
 *
 * The tables come from the root pointer the port already reads for the
 * processors (src/cpu.c, port_x86_64_acpi_table): the RSDP, the XSDT (or
 * the RSDT of an older firmware) and the FADT, which is the table "FACP".
 * Nothing of that is required for the machine to boot, so a firmware
 * without the tables costs a fallback and not a boot:
 *
 *   QEMU's PIIX4 answers the sleep request on the port 0x604 with the
 *   value 0x2000, which is the S5 of a machine whose PM1a control block
 *   is 0x604; Bochs answers 0xB004 the same way. Both writes are harmless
 *   on a machine that has neither, which is why they are tried before the
 *   port gives up and stops the processor with a sentence for the person
 *   at the keyboard.
 *
 * The reset is the reset register of the FADT when it has one (ACPI 6.x,
 * 4.8.3.6 - an I/O port or a byte of memory, a value), and the reset
 * control port of the chipset (0xCF9) of every PC made since the 1990s
 * otherwise. The two are tried in that order: a firmware that named a
 * register meant it, and a machine that ignored it is still a machine with
 * a chipset.
 *
 * One thing is deliberately not here: the SLP_TYPa of S5 comes from the
 * DSDT of the machine, an AML program this port does not execute. The
 * value 5 is what every machine the project has met uses - QEMU, Bochs and
 * the laptops of the cleanroom - and a machine that names another one is a
 * line of docs/debts.md, not a reason to leave the port without a soft off.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "port_x86_64_acpi.h"

/* The FADT of the specification (ACPI 6.x, table 5.10), the fields read. */
#define FADT_PM1A_CNT_BLK 0x40u /* the control block of PM1a, 4 bytes, an I/O port */
#define FADT_PM1B_CNT_BLK 0x44u /* the same of PM1b, 0 when the machine has none */
#define FADT_RESET_REG 0x74u    /* a GAS of 12 bytes */
#define FADT_RESET_VALUE 0x80u  /* the byte the reset register takes */
#define FADT_X_PM1A_CNT_BLK 0xABu
#define FADT_X_PM1B_CNT_BLK 0xB7u
#define FADT_LENGTH_PM1 0x48u
#define FADT_LENGTH_RESET 0x81u

/* The GAS of a reset register: an address space (0 memory, 1 I/O), a width,
 * an offset and an access size, then the address. */
#define GAS_SPACE_MEMORY 0u
#define GAS_SPACE_IO 1u

/* The sleep type of S5 and the sleep enable bit of the PM1 control. */
#define PM1_SLP_TYP_S5 (5u << 10)
#define PM1_SLP_EN (1u << 13)

/* The reset control port of the chipset and its two writes. */
#define RESET_CONTROL_PORT 0xCF9u
#define RESET_CONTROL_SYSTEM 0x02u
#define RESET_CONTROL_FULL 0x06u

/* The two ports of the emulators, and the value of the S5 request. */
#define QEMU_PM1A_CNT_PORT 0x604u
#define BOCHS_PM1A_CNT_PORT 0xB004u
#define ACPI_S5_REQUEST 0x2000u

/* The byte of I/O port 0x80, the delay port of every PC: the write of the
 * sleep request needs the chipset to see it before the processor reads the
 * next instruction, and a few hundred of these are a safe wait. */
#define ACPI_DELAY_PORT 0x80u
#define ACPI_DELAY_LOOPS 100000u

/* The table of the machine and its length, read once: the FADT does not
 * change and the walk of the root pointer is a scan of the firmware area. */
static const k_u8_t *g_fadt;
static k_u32_t g_fadt_length;
static bool g_fadt_read;

static k_u32_t read32(const k_u8_t *table, k_u32_t offset)
{
    k_u32_t value;

    memcpy(&value, table + offset, sizeof(value));
    return value;
}

static k_u64_t read64(const k_u8_t *table, k_u32_t offset)
{
    k_u64_t value;

    memcpy(&value, table + offset, sizeof(value));
    return value;
}

static const k_u8_t *fadt(void)
{
    if (!g_fadt_read) {
        k_u32_t length = 0;
        const void *table = port_x86_64_acpi_table("FACP", &length);

        g_fadt = (const k_u8_t *)table;
        g_fadt_length = length;
        g_fadt_read = true;

        if (g_fadt == NULL) {
            kinfo("acpi: the firmware has no FADT, the power of the machine is a fallback");
        } else {
            kinfo("acpi: the FADT of the machine is %u bytes", (unsigned)length);
        }
    }

    return g_fadt;
}

/*
 * The control block of a PM1: the 64-bit GAS of a firmware of ACPI 2.0 and
 * later, the 32-bit port of the older one. Zero when the machine has none
 * or names an address space this port cannot write.
 */
static k_u16_t pm1_control_block(k_u32_t old_offset, k_u32_t gas_offset)
{
    k_u32_t port;

    if (g_fadt_length >= gas_offset + 12u) {
        k_u8_t space = g_fadt[gas_offset];
        k_u64_t address = read64(g_fadt, gas_offset + 4u);

        if (address != 0u) {
            if (space == GAS_SPACE_IO && address <= 0xFFFFu) {
                return (k_u16_t)address;
            }

            kwarn("acpi: the control block at 0x%x is not an I/O port of this machine",
                  (unsigned)address);
            return 0;
        }
    }

    if (g_fadt_length < old_offset + 4u) {
        return 0;
    }

    port = read32(g_fadt, old_offset);

    return (port != 0u && port <= 0xFFFFu) ? (k_u16_t)port : 0;
}

static void acpi_delay(void)
{
    k_u32_t i;

    for (i = 0; i < ACPI_DELAY_LOOPS; i++) {
        (void)port_inb(ACPI_DELAY_PORT);
    }
}

int port_x86_64_acpi_off(void)
{
    k_u16_t pm1a;
    k_u16_t pm1b;

    if (fadt() == NULL) {
        return -1;
    }

    if (g_fadt_length < FADT_LENGTH_PM1) {
        kwarn("acpi: the FADT is too short for the control block of PM1a");
        return -1;
    }

    pm1a = pm1_control_block(FADT_PM1A_CNT_BLK, FADT_X_PM1A_CNT_BLK);
    if (pm1a == 0u) {
        return -1;
    }

    pm1b = pm1_control_block(FADT_PM1B_CNT_BLK, FADT_X_PM1B_CNT_BLK);

    kinfo("acpi: S5 on the port 0x%x", (unsigned)pm1a);
    if (pm1b != 0u) {
        kinfo("acpi: PM1b is 0x%x", (unsigned)pm1b);
    }

    /* The two blocks are written one after the other; a machine that obeys
     * the first does not reach the second. */
    port_outw(pm1a, (k_u16_t)(PM1_SLP_TYP_S5 | PM1_SLP_EN));

    if (pm1b != 0u) {
        port_outw(pm1b, (k_u16_t)(PM1_SLP_TYP_S5 | PM1_SLP_EN));
    }

    /* The machine of a firmware that lied about S5 is still here. */
    acpi_delay();

    return 0;
}

int port_x86_64_acpi_reset(void)
{
    bool wrote = false;

    if (fadt() != NULL && g_fadt_length >= FADT_LENGTH_RESET) {
        k_u8_t space = g_fadt[FADT_RESET_REG];
        k_u64_t address = read64(g_fadt, FADT_RESET_REG + 4u);
        k_u8_t value = g_fadt[FADT_RESET_VALUE];

        if (address != 0u && space == GAS_SPACE_IO && address <= 0xFFFFu) {
            kinfo("acpi: reset by the register of the FADT, the port 0x%x", (unsigned)address);
            port_outb((k_u16_t)address, value);
            wrote = true;
        } else if (address != 0u && space == GAS_SPACE_MEMORY && address < 0x40000000u) {
            /* The identity map of the low gigabyte covers the byte of the
             * firmware that the FADT names on a machine of ACPI 2.0. */
            kinfo("acpi: reset by the register of the FADT, the byte 0x%x", (unsigned)address);
            *(volatile k_u8_t *)(uintptr_t)address = value;
            wrote = true;
        } else if (address != 0u) {
            kwarn("acpi: the reset register at 0x%x is not one this port writes",
                  (unsigned)address);
        }

        acpi_delay();
    }

    /*
     * The reset control port of the chipset: bit 1 is the system reset and
     * bit 2 makes it a full reset of the processor as well. The sequence is
     * the one of the datasheets of the chipsets this port has met.
     */
    kinfo("acpi: reset by the control port 0x%x", (unsigned)RESET_CONTROL_PORT);
    port_outb(RESET_CONTROL_PORT, RESET_CONTROL_SYSTEM);
    port_outb(RESET_CONTROL_PORT, RESET_CONTROL_FULL);
    wrote = true;

    acpi_delay();

    return wrote ? 0 : -1;
}

const char *port_x86_64_acpi_name(void)
{
    return fadt() != NULL ? "acpi" : "none";
}
