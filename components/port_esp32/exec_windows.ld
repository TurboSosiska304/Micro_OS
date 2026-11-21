/*
 * Micro OS — the windows of a program of the board (ADR 0018).
 *
 * A program is linked for two fixed addresses and the port copies its image
 * there: the text in the instruction RAM at 0x4009E000 (8 KiB below the top
 * of the segment), the data at 0x3FFCC000 (the top of the static DRAM
 * segment, which the build fixes with CONFIG_ESP32_USE_FIXED_STATIC_RAM_SIZE
 * so that the heap begins at 0x3FFCE000 and never reaches it).
 *
 * The windows are not sections of the kernel: they are addresses of the
 * machine, and nothing in the linker script of ESP-IDF knows that a program
 * will be copied there. This script is the contract that keeps them free. The
 * linker places the static sections of the kernel bottom-up and ends them at
 * _iram_end and _bss_end; the assertion refuses a build whose own memory grew
 * into a window, and the fix is to shrink the window or the kernel, never to
 * copy a program over a variable of the system.
 *
 * GNU ld resolves the symbols of an ASSERT after every script has been read,
 * so the order in which this file reaches the linker does not matter.
 */

ASSERT(_iram_end <= 0x4009E000,
       "Micro OS: IRAM reaches the 8 KiB window of the programs at 0x4009E000")

ASSERT(_bss_end <= 0x3FFCC000,
       "Micro OS: DRAM reaches the 8 KiB window of the program data at 0x3FFCC000")

/*
 * The window of the data is outside the heap only when the size of the static
 * DRAM segment is fixed (memory.ld sets _heap_start to the end of that
 * segment). Without CONFIG_ESP32_USE_FIXED_STATIC_RAM_SIZE the heap starts at
 * _bss_end, runs through 0x3FFCC000 and would be handed to a task on top of a
 * program. A stale sdkconfig is caught here instead of on the board.
 */
ASSERT(_heap_start >= 0x3FFCE000,
       "Micro OS: CONFIG_ESP32_USE_FIXED_STATIC_RAM_SIZE=0x1E000 is not in sdkconfig")
