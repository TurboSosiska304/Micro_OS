# Micro OS — ESP32 platform port (ESP-IDF component).

set(MICRO_OS_PORT_ESP32_SRCS
    "src/port_esp32.c"
    "src/port_exec.c"
    "src/programs.c"
    "src/blockdev_flash.c"
    "src/port_kv_nvs.c"
    "src/port_net_lwip.c"
)

# The radio is the backend of the portable core (MICRO_OS_WIFI, ADR 0010):
# it fills the table of `wifi` and reports its events back to it, and nothing
# of that appears in the header of the port. Only the source follows the
# switch: the requirements of a component are read by ESP-IDF before the
# configuration is known, and a REQUIRES line that depends on CONFIG_ is
# empty in that pass.
if(CONFIG_MICRO_OS_WIFI)
    list(APPEND MICRO_OS_PORT_ESP32_SRCS "src/wifi_radio.c")
endif()

idf_component_register(
    SRCS ${MICRO_OS_PORT_ESP32_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel esp_timer freertos esp_driver_uart vfs esp_partition nvs_flash lwip
             esp_wifi esp_netif esp_event
    # The radio is the backend of the portable core: it fills the table of
    # `wifi` and reports its events back to it, and nothing of that appears
    # in the header of the port. `heap` is for SOC_RESERVE_MEMORY_REGION: the
    # port keeps the IRAM window of a program out of the heap (ADR 0018).
    PRIV_REQUIRES wifi heap
    # The port is the other half of the kernel contract: the kernel calls
    # k_port_* and the port calls back into the kernel (the scheduler, the
    # log). ESP-IDF links the components in dependency order and repeats
    # them to break cycles, but a member of an archive is only pulled when
    # something already needs it - so the files that only the kernel calls
    # (the store, the sockets) would be dropped and the link would fail.
    # The port is small; it is linked whole.
    WHOLE_ARCHIVE
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)

# --- the programs of the board (ADR 0018) -------------------------------------
#
# A program is built from the same C library as a program of x86-64
# (components/libc/user) with the Xtensa compiler of ESP-IDF and the windowed
# ABI of the machine, linked for the two windows of the port
# (programs/program_esp32.ld), turned into a MOX1 image of arch 1 by
# tools/mkprog.sh and embedded in the firmware as a blob. The kernel needs the
# bytes before it has a filesystem to read them from; at the boot they are
# written to /bin and run through execve.
#
# The text and the data are two windows because the instruction RAM of the
# ESP32 is only 32-bit accessible for data: the code runs from IRAM and the
# strings of the program are in DRAM. Both numbers are in port_exec.c and in
# exec_windows.ld; mkprog.sh refuses an image linked for other ones.
set(MICRO_OS_PROGRAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/programs")
set(MICRO_OS_LIBC_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libc/user")
set(MICRO_OS_KERNEL_INC "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include")
set(MICRO_OS_MKPROG "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mkprog.sh")

set(MICRO_OS_PROGRAM_CFLAGS
    -c -Os -ffreestanding -fno-stack-protector
    -fno-pic -fno-pie -mlongcalls -mtext-section-literals
    -fno-asynchronous-unwind-tables -fno-merge-constants
    -fno-builtin-memcpy -fno-builtin-memset
    -Wall -Wextra -Werror
    -DMOX_ARCH_XTENSA=1
    -I "${MICRO_OS_KERNEL_INC}" -I "${MICRO_OS_LIBC_USER_DIR}/include"
)

# Every program of the port is built the same way: compile the file and the
# four sources of the library for the windows, link with the script of the
# format, turn the result into a MOX1 image of arch 1 and embed the image in
# the firmware as a blob. The name of the file is the name in /bin.
function(add_board_program name entry)
    set(elf "${CMAKE_CURRENT_BINARY_DIR}/${name}.elf")
    set(mox "${CMAKE_CURRENT_BINARY_DIR}/${name}.mox")

    add_custom_command(
        OUTPUT "${mox}"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_start.o "${MICRO_OS_LIBC_USER_DIR}/src/start.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_syscall.o "${MICRO_OS_LIBC_USER_DIR}/src/syscall.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_stdio.o "${MICRO_OS_LIBC_USER_DIR}/src/stdio.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_string.o "${MICRO_OS_LIBC_USER_DIR}/src/string.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_main.o "${MICRO_OS_PROGRAM_DIR}/${name}.c"
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
                -Wl,-e,${entry} -T "${MICRO_OS_PROGRAM_DIR}/program_esp32.ld"
                -o ${name}.elf ${name}_start.o ${name}_syscall.o ${name}_stdio.o
                ${name}_string.o ${name}_main.o
        COMMAND ${CMAKE_COMMAND} -E env
                "OBJCOPY=${CMAKE_OBJCOPY}" "OBJDUMP=${CMAKE_OBJDUMP}" "NM=${CMAKE_NM}"
                sh "${MICRO_OS_MKPROG}" --base 0x4009E000 --data-base 0x3FFCE000
                --arch xtensa ${name}.elf ${name}.mox ${entry}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${MICRO_OS_PROGRAM_DIR}/${name}.c" "${MICRO_OS_PROGRAM_DIR}/program_esp32.ld"
                "${MICRO_OS_LIBC_USER_DIR}/src/start.c" "${MICRO_OS_LIBC_USER_DIR}/src/syscall.c"
                "${MICRO_OS_LIBC_USER_DIR}/src/stdio.c" "${MICRO_OS_LIBC_USER_DIR}/src/string.c"
                "${MICRO_OS_LIBC_USER_DIR}/include/ulibc.h"
                "${MICRO_OS_KERNEL_INC}/kernel/ksys.h" "${MICRO_OS_MKPROG}"
        COMMENT "building the image of /bin/${name} for the board"
        VERBATIM
    )

    # The symbols of the blob are _binary_<name>_mox_start and _end.
    target_add_binary_data(${COMPONENT_LIB} "${mox}" BINARY DEPENDS "${mox}")
endfunction()

add_board_program(hello _start)
add_board_program(cat _start)

# The two windows of a program are not sections of the kernel: they are the
# addresses a program is linked for and the port copies the image there. The
# assertion makes the linker refuse a kernel that grew into them instead of
# letting a program overwrite a variable of the system. It is a script of its
# own and not a line of the port, because only the linker knows where its own
# segments ended.
idf_build_set_property(LINK_OPTIONS
    "-T${CMAKE_CURRENT_SOURCE_DIR}/exec_windows.ld" APPEND)

# The linker reads the file above, so a change to it has to relink the image
# (and a stale sdkconfig has to fail the link, not the board).
idf_build_set_property(__LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/exec_windows.ld" APPEND)
