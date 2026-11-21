# Micro OS — the x86-64 port as a project of its own.
#
# There is no libc, no POSIX and no operating system under the kernel. Two
# images are built:
#
#   microos64.bin  the kernel itself, linked at 16 MiB, a flat binary;
#   loader32.elf   the bootloader of the port, a multiboot image that QEMU
#                  loads with -kernel, which copies the kernel and turns the
#                  long mode on.
#
#   cmake -B build-x86 -S ports/port_x86_64 -G Ninja
#   cmake --build build-x86
#   qemu-system-x86_64 -kernel build-x86/loader32.elf \
#                      -initrd build-x86/microos64.bin -m 256 \
#                      -display gtk -serial none

cmake_minimum_required(VERSION 3.16)

project(micro_os_x86_64 C ASM)

set(KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kernel")
set(KSHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kshell")
set(LIBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/libc")
set(MFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/mfs")
set(FAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/fs_fat32")

# The second filesystem of the machine (ADR 0009). Off by default: the
# disk of `make x86-smoke` is one MicroFS volume, and a reader that no call
# of the boot asks for is an object file of no use. A machine with a FAT
# partition turns it on and mounts the partition with vfs_mount_type().
option(MICRO_OS_FS_FAT32 "FAT12/16/32, read and written" OFF)

# The optional features of the image (ADR 0010): the PSRAM pool of the
# allocator is off, the MOX1 runtime is on (the port can enter a program), and
# the two wireless services of the board are off. Their flags are written
# here so that the defaults of the port are in one place.
option(KERNEL_PSRAM "the PSRAM pool and its statistics in kmm" OFF)
option(KERNEL_EXEC "the MOX1 parser, execve and the exec command" ON)
option(MICRO_OS_TELNET "the shell on TCP port 23" OFF)
option(MICRO_OS_WIFI "the WiFi station" OFF)

# The network of the machine (K6): the port has its own IPv4 stack and a
# virtio-net driver (src/net.c, src/virtio_net.c), which are always part of
# the image and cost nothing when QEMU was given no card. This switch adds
# the commands of components/net - `http`, `httpd` and `ntp` - on top of that
# stack. It is off by default: the smoke test of the machine keeps the same
# command list, and `make x86-net-smoke` builds a second image with it on.
option(MICRO_OS_X86_NET "the http, httpd and ntp commands of components/net" OFF)

set(NET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/net")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The kernel of the low memory: no red zone (an interrupt may use the stack
# under us), no PIC, no SSE (there is no floating point state to save), and
# the small memory model, which the identity map of the loader expects.
add_compile_options(
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mgeneral-regs-only
    # The machine is the first x86-64 and not the computer of the person who
    # builds it. A distribution may configure its compiler for a newer model
    # (the runner of GitHub does) and then a shift becomes SHLX of BMI2,
    # which the processor of QEMU does not know: an invalid opcode in the
    # middle of the boot. The explicit model is the oldest one.
    -march=x86-64
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    # The kernel has no libc, and some distributions turn the checks of the
    # C library into calls to __snprintf_chk and friends: there is nobody to
    # answer them under the kernel.
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
)

file(GLOB KERNEL_SOURCES "${KERNEL_DIR}/src/*.c")

# The shell is the kernel's own interpreter: the port supplies the console
# under it and nothing else, so the commands are the same as on the board.
set(KSHELL_SOURCES
    "${KSHELL_DIR}/src/kshell.c"
    "${KSHELL_DIR}/src/kshell_files.c"
    "${KSHELL_DIR}/src/kshell_memtest.c"
    "${KSHELL_DIR}/src/kshell_mfetch.c"
)

# The parts of the C library the shell calls. They reach the kernel through
# the ABI of the system calls and know nothing about the port.
#
# src/stdio.c is left out on purpose: the path of its %f conversions needs
# SSE registers, and the port is built with -mgeneral-regs-only because
# nothing here saves the XMM state. The formatter of src/libc_shim.c serves
# the shell instead; it speaks the conversions the commands use, without the
# precision and the '*' widths that src/stdio.c adds.
set(LIBC_SOURCES
    "${LIBC_DIR}/src/stdlib.c"
    "${LIBC_DIR}/src/unistd.c"
)

# MicroFS, so that the machine has a tree of directories and a program in
# /bin. The filesystem is the same code the board runs, mounted on the RAM
# volume the port registers (src/blockdev_ram.c). src/mfs_ram_device.c is not
# here: that one is the volume of the host tests, and it takes its memory from
# the libc of the process.
set(MFS_SOURCES
    "${MFS_DIR}/src/mfs.c"
    "${MFS_DIR}/src/mfs_check.c"
    "${MFS_DIR}/src/mfs_vfs.c"
    "${MFS_DIR}/src/mfs_vfs_mount.c"
)

set(FAT_SOURCES)
if(MICRO_OS_FS_FAT32)
    set(FAT_SOURCES
        "${FAT_DIR}/src/fat_core.c"
        "${FAT_DIR}/src/fat_dir.c"
        "${FAT_DIR}/src/fat_vfs.c"
    )
endif()

# The services over the sockets: the HTTP client, the page of the state and
# the clock of NTP. They are portable and speak kernel/net.h, so the same
# sources that run on the board run here over the stack of the port. The
# clients are off by default (MICRO_OS_X86_NET); the sources are named here
# and not by the CMakeLists of the component, which is written for the tree of
# ESP-IDF. src/dns.c is the exception: the stack of the port asks it for every
# name, in every image, and it is the cache the contract of K6 asks for.
set(NET_SOURCES "${NET_DIR}/src/dns.c")

if(MICRO_OS_X86_NET)
    list(APPEND NET_SOURCES
        "${NET_DIR}/src/net.c"
        "${NET_DIR}/src/http.c"
        "${NET_DIR}/src/ntp.c"
        "${NET_DIR}/src/net_http_server.c"
    )
endif()

# The memory test calls strtoul without including the header of the library;
# on the board that resolves to the libc of ESP-IDF. Here the name has to
# point at the conversion of the system, so the header is put in front of
# that one file and its macros do the renaming.
set_source_files_properties("${KSHELL_DIR}/src/kshell_memtest.c"
    PROPERTIES COMPILE_OPTIONS "-include;libc/stdlib.h")

# --- the program of the port: /bin/hello -------------------------------------
#
# The image has to be inside the kernel before there is a filesystem to read it
# from, so it is built here and linked in as a blob. Three steps, and the
# second one is the interesting one: tools/mkprog.sh copies the sections of an
# object as they are, and a reference to a string is a relocation until a
# linker has applied it. The object is therefore linked first, for the window
# of the programs (programs/program.ld), and the image is made out of the
# linked file.
#
# program.ld puts the text at the base of the window plus the header of the
# format (0x02000020) and SUBALIGN(4) keeps the 64-byte alignment of the
# object from moving it. The base is named here again and handed to mkprog.sh,
# which refuses an image linked for another window: a program linked for the
# wrong address would still run, and every absolute address it computes for
# its data would name something else.
set(PROGRAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/programs")
set(PROGRAM_BASE 0x02000000)
set(MKPROG "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mkprog.sh")

# The library of a program (components/libc/user): `_start`, the door of the
# syscall, the formatter and the strings. It is not the library of the kernel
# (${LIBC_DIR}/src), which is linked into the kernel and calls ksys_*: a
# program has no such address, only `int $0x80`. Every program that uses it
# carries its own copy, which is what "statically linked" means here.
set(LIBC_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/libc/user")
set(LIBC_USER_SOURCES
    "${LIBC_USER_DIR}/src/start.c"
    "${LIBC_USER_DIR}/src/syscall.c"
    "${LIBC_USER_DIR}/src/stdio.c"
    "${LIBC_USER_DIR}/src/string.c"
)

# The flags of a program: no libc, no PIC and no SSE, the same shape as the
# kernel's (see add_compile_options above). The include path carries the
# headers of the kernel (the numbers of the calls) and the library.
set(PROGRAM_CFLAGS
    -c -O2 -ffreestanding -fno-stack-protector
    -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -march=x86-64
    -fno-asynchronous-unwind-tables -Wall -Wextra -Werror
    -I "${KERNEL_DIR}/include" -I "${PROGRAM_DIR}" -I "${LIBC_USER_DIR}/include"
)

# Every program of the port is built the same way: compile the C file for the
# window, link it with the script of the format, turn the sections into a MOX1
# image and embed the image in the kernel as a blob. The name of the function
# is the name of the file in /bin and the symbol of the entry is given here.
function(add_program_image name entry)
    set(image "${CMAKE_BINARY_DIR}/${name}.mox")
    set(object "${CMAKE_BINARY_DIR}/${name}_mox.o")

    add_custom_command(
        OUTPUT "${image}"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}.o "${PROGRAM_DIR}/${name}.c"
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
                -Wl,-e,${entry}
                -T "${PROGRAM_DIR}/program.ld" -o ${name}.elf ${name}.o
        COMMAND sh "${MKPROG}" --base ${PROGRAM_BASE} ${name}.elf ${name}.mox ${entry}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${PROGRAM_DIR}/${name}.c" "${PROGRAM_DIR}/prog.h"
                "${PROGRAM_DIR}/program.ld" "${KERNEL_DIR}/include/kernel/ksys.h" "${MKPROG}"
        COMMENT "building the image of /bin/${name}"
        VERBATIM
    )

    # The blob goes to .rodata, which the linker script of the port keeps in
    # the image that the loader copies; objcopy names its symbols after the
    # file: _binary_<name>_mox_start and _end.
    add_custom_command(
        OUTPUT "${object}"
        COMMAND objcopy -I binary -O elf64-x86-64
                --rename-section .data=.rodata,alloc,load,readonly,data,contents
                ${name}.mox ${name}_mox.o
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${image}"
        COMMENT "embedding the image of /bin/${name}"
        VERBATIM
    )

    set_source_files_properties("${object}" PROPERTIES
        GENERATED TRUE
        EXTERNAL_OBJECT TRUE
    )

    set(${name}_OBJECT "${object}" PARENT_SCOPE)
endfunction()

add_program_image(hello hello_main)
add_program_image(crash crash_main)
add_program_image(fork fork_main)
add_program_image(badptr badptr_main)

# A program that uses the library of a program: the runtime sources are
# compiled for the window together with the file of the program, linked with
# the same script, and the entry is `_start` of the library - main() is a
# function of the program, not the place the port enters (ADR 0016).
function(add_libc_program name)
    set(image "${CMAKE_BINARY_DIR}/${name}.mox")
    set(object "${CMAKE_BINARY_DIR}/${name}_mox.o")
    set(objects
        "${name}_start.o"
        "${name}_syscall.o"
        "${name}_stdio.o"
        "${name}_string.o"
        "${name}.o"
    )

    add_custom_command(
        OUTPUT "${image}"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}_start.o "${LIBC_USER_DIR}/src/start.c"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}_syscall.o "${LIBC_USER_DIR}/src/syscall.c"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}_stdio.o "${LIBC_USER_DIR}/src/stdio.c"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}_string.o "${LIBC_USER_DIR}/src/string.c"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}.o "${PROGRAM_DIR}/${name}.c"
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
                -Wl,-e,_start
                -T "${PROGRAM_DIR}/program.ld" -o ${name}.elf ${objects}
        COMMAND sh "${MKPROG}" --base ${PROGRAM_BASE} ${name}.elf ${name}.mox _start
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${PROGRAM_DIR}/${name}.c" "${PROGRAM_DIR}/program.ld"
                "${KERNEL_DIR}/include/kernel/ksys.h" "${MKPROG}"
                ${LIBC_USER_SOURCES} "${LIBC_USER_DIR}/include/ulibc.h"
        COMMENT "building the image of /bin/${name} with the library of a program"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${object}"
        COMMAND objcopy -I binary -O elf64-x86-64
                --rename-section .data=.rodata,alloc,load,readonly,data,contents
                ${name}.mox ${name}_mox.o
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${image}"
        COMMENT "embedding the image of /bin/${name}"
        VERBATIM
    )

    set_source_files_properties("${object}" PROPERTIES
        GENERATED TRUE
        EXTERNAL_OBJECT TRUE
    )

    set(${name}_OBJECT "${object}" PARENT_SCOPE)
endfunction()

add_libc_program(cat)
add_libc_program(echo)
add_libc_program(ls)
add_libc_program(spawn)

add_executable(microos64
    ${KERNEL_SOURCES}
    ${KSHELL_SOURCES}
    ${LIBC_SOURCES}
    ${MFS_SOURCES}
    ${FAT_SOURCES}
    ${NET_SOURCES}
    src/boot64.S
    src/kmain.c
    src/shell.c
    src/port_x86_64.c
    src/vga.c
    src/ps2.c
    src/gdt.c
    src/paging.c
    src/port_exec.c
    src/port_x86_64_task.c
    src/blockdev_ram.c
    src/ata.c
    src/virtio_net.c
    src/net.c
    src/libc_shim.c
    src/interrupts.S
    src/switch.S
    "${hello_OBJECT}"
    "${crash_OBJECT}"
    "${fork_OBJECT}"
    "${badptr_OBJECT}"
    "${cat_OBJECT}"
    "${echo_OBJECT}"
    "${ls_OBJECT}"
    "${spawn_OBJECT}"
)

target_include_directories(microos64 PRIVATE
    "${KERNEL_DIR}/include"
    "${KSHELL_DIR}/include"
    "${LIBC_DIR}/include"
    "${MFS_DIR}/include"
    "${FAT_DIR}/include"
    "${NET_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(microos64 PRIVATE
    KERNEL_BOARD_NAME="x86_64"
    KERNEL_BOARD_DESC="QEMU x86-64 machine"
    KERNEL_PSRAM=$<BOOL:${KERNEL_PSRAM}>
    KERNEL_EXEC=$<BOOL:${KERNEL_EXEC}>
    MICRO_OS_NET=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_HTTP=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_HTTP_SERVER=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_NTP=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_MQTT=0
    MICRO_OS_TELNET=$<BOOL:${MICRO_OS_TELNET}>
    MICRO_OS_WIFI=$<BOOL:${MICRO_OS_WIFI}>
)

if(MICRO_OS_FS_FAT32)
    target_compile_definitions(microos64 PRIVATE MICRO_OS_FS_FAT32=1)
endif()

set_target_properties(microos64 PROPERTIES
    LINK_FLAGS "-nostdlib -static -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker64.ld -Wl,--build-id=none"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/microos64.bin"
    COMMAND objcopy -O binary "$<TARGET_FILE:microos64>" "${CMAKE_BINARY_DIR}/microos64.bin"
    DEPENDS microos64
    COMMENT "the flat image of the kernel"
    VERBATIM
)

# The loader is 32-bit and has no C in it: the assembler and the linker of
# the host do the whole job.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/loader32.o"
    COMMAND as --32 -o "${CMAKE_BINARY_DIR}/loader32.o" "${CMAKE_CURRENT_SOURCE_DIR}/loader32.S"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loader32.S"
    COMMENT "assembling the loader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/loader32.elf"
    COMMAND ld -m elf_i386 -T "${CMAKE_CURRENT_SOURCE_DIR}/loader32.ld"
            -o "${CMAKE_BINARY_DIR}/loader32.elf" "${CMAKE_BINARY_DIR}/loader32.o"
    DEPENDS "${CMAKE_BINARY_DIR}/loader32.o"
    COMMENT "linking the loader"
    VERBATIM
)

add_custom_target(image
    DEPENDS "${CMAKE_BINARY_DIR}/microos64.bin" "${CMAKE_BINARY_DIR}/loader32.elf"
    COMMENT "the image of the port: loader32.elf + microos64.bin"
)
