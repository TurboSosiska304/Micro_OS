# Micro OS — ESP32 platform port (ESP-IDF component).

set(MICRO_OS_PORT_ESP32_SRCS
    "src/port_esp32.c"
    "src/port_exec.c"
    "src/programs.c"
    "src/progs_vfs.c"
    "src/progs_format.c"
    "src/ota.c"
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

# --- the signature of the application (ADR 0020) -----------------------------
#
# The same parser, hash and curve as the x86-64 loader: the board reads its
# own application slot in app_main, before the filesystem and the shell, and
# compares the trailer at the last 4 KiB of the slot with the bytes of the
# app. The public key is compiled into the firmware, so the image cannot
# bring one of its own.
#
# The tool that writes sign_public_key.h is built for the host: ESP-IDF has a
# cross compiler for the board, and the tool is an ordinary C program with a
# main(). The key is MICROOS_SIGN_KEY (a path outside the tree) or the
# development key in the build directory, the same choice as the x86-64 port.
set(MICRO_OS_SIGN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ports/port_x86_64/loader")
set(MICRO_OS_SIGN_SHARED
    "${MICRO_OS_SIGN_DIR}/sha256.c"
    "${MICRO_OS_SIGN_DIR}/ed25519.c"
    "${MICRO_OS_SIGN_DIR}/sign_image.c"
)
list(APPEND MICRO_OS_PORT_ESP32_SRCS "src/image_check.c" ${MICRO_OS_SIGN_SHARED})

# The first pass of the build only asks the components for their requirements
# and cannot create rules, so the tool and its header exist in the real pass.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)

find_program(MICRO_OS_HOST_CC NAMES cc gcc clang)
if(NOT MICRO_OS_HOST_CC)
    message(FATAL_ERROR "Micro OS: a host C compiler is needed for the signing key of the image")
endif()

# The tool and the key live in the root of the build directory, beside
# micro_os.bin: tools/bundle.sh and tools/device-test.sh find them there by
# the same names the x86-64 build uses.
set(MICRO_OS_SIGN_TOOL "${CMAKE_BINARY_DIR}/microos_sign_host")
set(MICRO_OS_SIGN_HEADER "${CMAKE_BINARY_DIR}/sign_public_key.h")
set(MICRO_OS_SIGN_PUBLIC "${CMAKE_BINARY_DIR}/signing.pub")

add_custom_command(
    OUTPUT "${MICRO_OS_SIGN_TOOL}"
    COMMAND "${MICRO_OS_HOST_CC}" -O2
            -I "${MICRO_OS_SIGN_DIR}"
            -o "${MICRO_OS_SIGN_TOOL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/microos_sign.c"
            ${MICRO_OS_SIGN_SHARED}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/microos_sign.c"
            ${MICRO_OS_SIGN_SHARED}
    COMMENT "building the host tool of the image signature"
    VERBATIM
)

if(MICROOS_SIGN_KEY)
    set(MICRO_OS_SIGN_KEY_FILE "${MICROOS_SIGN_KEY}")
    add_custom_command(
        OUTPUT "${MICRO_OS_SIGN_HEADER}" "${MICRO_OS_SIGN_PUBLIC}"
        COMMAND "${MICRO_OS_SIGN_TOOL}" pubkey
                --key "${MICRO_OS_SIGN_KEY_FILE}"
                --pub "${MICRO_OS_SIGN_PUBLIC}"
                --header "${MICRO_OS_SIGN_HEADER}"
        DEPENDS "${MICRO_OS_SIGN_TOOL}" "${MICRO_OS_SIGN_KEY_FILE}"
        COMMENT "reading the public half of the signing key"
        VERBATIM
    )
else()
    set(MICRO_OS_SIGN_KEY_FILE "${CMAKE_BINARY_DIR}/signing.key")
    add_custom_command(
        OUTPUT "${MICRO_OS_SIGN_KEY_FILE}" "${MICRO_OS_SIGN_HEADER}" "${MICRO_OS_SIGN_PUBLIC}"
        COMMAND sh -c "test -f '${MICRO_OS_SIGN_KEY_FILE}' || '${MICRO_OS_SIGN_TOOL}' keygen --key '${MICRO_OS_SIGN_KEY_FILE}' --pub '${MICRO_OS_SIGN_PUBLIC}' --header '${MICRO_OS_SIGN_HEADER}'"
        COMMAND "${MICRO_OS_SIGN_TOOL}" pubkey
                --key "${MICRO_OS_SIGN_KEY_FILE}"
                --pub "${MICRO_OS_SIGN_PUBLIC}"
                --header "${MICRO_OS_SIGN_HEADER}"
        DEPENDS "${MICRO_OS_SIGN_TOOL}"
        COMMENT "generating the key of the image of this build"
        VERBATIM
    )
endif()

add_custom_target(micro_os_signing DEPENDS "${MICRO_OS_SIGN_HEADER}")

endif() # NOT CMAKE_BUILD_EARLY_EXPANSION

idf_component_register(
    SRCS ${MICRO_OS_PORT_ESP32_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${MICRO_OS_SIGN_DIR}" "${CMAKE_BINARY_DIR}"
    REQUIRES kernel esp_timer freertos esp_driver_uart vfs esp_partition nvs_flash lwip
             esp_wifi esp_netif esp_event
    # The radio is the backend of the portable core: it fills the table of
    # `wifi` and reports its events back to it, and nothing of that appears
    # in the header of the port. `heap` is for SOC_RESERVE_MEMORY_REGION: the
    # port keeps the IRAM window of a program out of the heap (ADR 0018).
    # esp_app_format is the description of the application (its version), and
    # it is private to the check of the image. app_update is the OTA slot of
    # the boot, the rollback and the confirmation (ADR 0022).
    PRIV_REQUIRES wifi heap esp_app_format app_update
    # The port is the other half of the kernel contract: the kernel calls
    # k_port_* and the port calls back into the kernel (the scheduler, the
    # log). ESP-IDF links the components in dependency order and repeats
    # them to break cycles, but a member of an archive is only pulled when
    # something already needs it - so the files that only the kernel calls
    # (the store, the sockets) would be dropped and the link would fail.
    # The port is small; it is linked whole.
    WHOLE_ARCHIVE
)

# The header of the key is generated, so the compiler has to wait for it.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} micro_os_signing)
endif()

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)

# --- the programs of the board (ADR 0018 §2, ADR 0022) ------------------------
#
# A program is built from the same C library as a program of x86-64
# (components/libc/user) with the Xtensa compiler of ESP-IDF and the windowed
# ABI of the machine, linked for the two windows of the port
# (programs/program_esp32.ld) and turned into a MOX1 image of arch 1 by
# tools/mkprog.sh. The image is *not* embedded in the firmware: the `.mox`
# files become the `progs` partition with tools/progs_image.py, the partition
# is flashed beside the application and mounted at /bin, and execve reads a
# program as a file of the VFS.
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
# format and turn the result into a MOX1 image of arch 1. The name of the
# file is the name in /bin; the image is collected into the partition below.
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
        # The wrappers of the calls keep the magnitude of a negative answer in
        # `errno` (src/errno.c), which the port of zlib to the library added;
        # the program of the board links the file as every program does.
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_errno.o "${MICRO_OS_LIBC_USER_DIR}/src/errno.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_errno.o "${MICRO_OS_LIBC_USER_DIR}/src/errno.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_errno.o "${MICRO_OS_LIBC_USER_DIR}/src/errno.c"
        COMMAND ${CMAKE_C_COMPILER} ${MICRO_OS_PROGRAM_CFLAGS}
                -o ${name}_main.o "${MICRO_OS_PROGRAM_DIR}/${name}.c"
        # -lgcc is the compiler's own runtime: the formatter of a program
        # computes a 64-bit number with a division and a remainder, and the
        # Xtensa has no 64-bit division instruction.
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
                -Wl,-e,${entry} -T "${MICRO_OS_PROGRAM_DIR}/program_esp32.ld"
                -o ${name}.elf ${name}_start.o ${name}_syscall.o ${name}_stdio.o
                ${name}_string.o ${name}_errno.o ${name}_main.o -lgcc
        COMMAND ${CMAKE_COMMAND} -E env
                "OBJCOPY=${CMAKE_OBJCOPY}" "OBJDUMP=${CMAKE_OBJDUMP}" "NM=${CMAKE_NM}"
                sh "${MICRO_OS_MKPROG}" --base 0x4009E000 --data-base 0x3FFCE000
                --arch xtensa ${name}.elf ${name}.mox ${entry}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${MICRO_OS_PROGRAM_DIR}/${name}.c" "${MICRO_OS_PROGRAM_DIR}/program_esp32.ld"
                "${MICRO_OS_LIBC_USER_DIR}/src/start.c" "${MICRO_OS_LIBC_USER_DIR}/src/syscall.c"
                "${MICRO_OS_LIBC_USER_DIR}/src/errno.c"
                "${MICRO_OS_LIBC_USER_DIR}/src/stdio.c" "${MICRO_OS_LIBC_USER_DIR}/src/string.c"
                "${MICRO_OS_LIBC_USER_DIR}/src/errno.c"
                "${MICRO_OS_LIBC_USER_DIR}/include/ulibc.h"
                "${MICRO_OS_KERNEL_INC}/kernel/ksys.h" "${MICRO_OS_MKPROG}"
        COMMENT "building the image of /bin/${name} for the board"
        VERBATIM
    )

    set(MICRO_OS_PROGRAM_IMAGES "${MICRO_OS_PROGRAM_IMAGES};${mox}" PARENT_SCOPE)
endfunction()

add_board_program(hello _start)
add_board_program(cat _start)

# The image of the `progs` partition: the directory of progs_format.h and the
# programs above, at their 64 KiB slots. The tool checks the names, the sizes
# and the alignment, so a program too big for a slot fails the build and not
# the board. It is flashed by `idf.py flash` (the partition `progs` of
# partitions.csv) and copied into the bundle by tools/bundle.sh.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_program(MICRO_OS_PYTHON NAMES python3 python)
    if(NOT MICRO_OS_PYTHON)
        message(FATAL_ERROR "Micro OS: a Python interpreter is needed for the image of the programs")
    endif()

    set(MICRO_OS_PROGS_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/progs.bin")
    add_custom_command(
        OUTPUT "${MICRO_OS_PROGS_IMAGE}"
        COMMAND "${MICRO_OS_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/progs_image.py"
                --output "${MICRO_OS_PROGS_IMAGE}"
                --partition-size 0x21000 --slot-size 0x10000
                "hello=${CMAKE_CURRENT_BINARY_DIR}/hello.mox"
                "cat=${CMAKE_CURRENT_BINARY_DIR}/cat.mox"
        DEPENDS ${MICRO_OS_PROGRAM_IMAGES}
                "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/progs_image.py"
                "${MICRO_OS_KERNEL_INC}/kernel/types.h"
        COMMENT "building the progs partition: /bin/hello and /bin/cat"
        VERBATIM
    )

    add_custom_target(micro_os_progs ALL DEPENDS "${MICRO_OS_PROGS_IMAGE}")

    # The partition travels with the firmware: `idf.py flash` writes the
    # application and the programs, so a board that was just flashed has a
    # /bin. A bundle names the same file (tools/bundle.sh) and flash.sh puts
    # it at the offset of partitions.csv.
    esptool_py_flash_to_partition(flash "progs" "${MICRO_OS_PROGS_IMAGE}")
endif()

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
