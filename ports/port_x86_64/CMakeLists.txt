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
#                      -initrd build-x86/microos64.bin -m 256 -nographic

cmake_minimum_required(VERSION 3.16)

project(micro_os_x86_64 C ASM)

set(KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kernel")
set(KSHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kshell")
set(LIBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/libc")
set(MFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/mfs")

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
    "${MFS_DIR}/src/mfs_vfs.c"
    "${MFS_DIR}/src/mfs_vfs_mount.c"
)

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
set(PROGRAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/programs")
set(PROGRAM_IMAGE "${CMAKE_BINARY_DIR}/hello.mox")
set(PROGRAM_OBJECT "${CMAKE_BINARY_DIR}/hello_mox.o")
set(MKPROG "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mkprog.sh")

add_custom_command(
    OUTPUT "${PROGRAM_IMAGE}"
    COMMAND ${CMAKE_C_COMPILER} -c -O2 -ffreestanding -fno-stack-protector
            -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only
            -fno-asynchronous-unwind-tables -Wall -Wextra -Werror
            -o hello.o "${PROGRAM_DIR}/hello.c"
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
            -Wl,-e,hello_main
            -T "${PROGRAM_DIR}/program.ld" -o hello.elf hello.o
    COMMAND sh "${MKPROG}" hello.elf hello.mox hello_main
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${PROGRAM_DIR}/hello.c" "${PROGRAM_DIR}/program.ld" "${MKPROG}"
    COMMENT "building the image of /bin/hello"
    VERBATIM
)

# The blob of the image as an object of the kernel. It goes to .rodata, which
# the linker script of the port keeps in the image that the loader copies, and
# objcopy names its symbols after the file: _binary_hello_mox_start and _end.
add_custom_command(
    OUTPUT "${PROGRAM_OBJECT}"
    COMMAND objcopy -I binary -O elf64-x86-64
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            hello.mox hello_mox.o
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${PROGRAM_IMAGE}"
    COMMENT "embedding the image of /bin/hello"
    VERBATIM
)

set_source_files_properties("${PROGRAM_OBJECT}" PROPERTIES
    GENERATED TRUE
    EXTERNAL_OBJECT TRUE
)

add_executable(microos64
    ${KERNEL_SOURCES}
    ${KSHELL_SOURCES}
    ${LIBC_SOURCES}
    ${MFS_SOURCES}
    src/boot64.S
    src/kmain.c
    src/shell.c
    src/port_x86_64.c
    src/port_exec.c
    src/port_x86_64_task.c
    src/blockdev_ram.c
    src/libc_shim.c
    src/interrupts.S
    src/switch.S
    "${PROGRAM_OBJECT}"
)

target_include_directories(microos64 PRIVATE
    "${KERNEL_DIR}/include"
    "${KSHELL_DIR}/include"
    "${LIBC_DIR}/include"
    "${MFS_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(microos64 PRIVATE
    KERNEL_BOARD_NAME="x86_64"
    KERNEL_BOARD_DESC="QEMU x86-64 machine"
)

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
