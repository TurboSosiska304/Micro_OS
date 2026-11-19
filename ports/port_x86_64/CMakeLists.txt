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
)

file(GLOB KERNEL_SOURCES "${KERNEL_DIR}/src/*.c")

add_executable(microos64
    ${KERNEL_SOURCES}
    src/boot64.S
    src/kmain.c
    src/port_x86_64.c
    src/port_x86_64_task.c
    src/libc_shim.c
)

target_include_directories(microos64 PRIVATE
    "${KERNEL_DIR}/include"
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
