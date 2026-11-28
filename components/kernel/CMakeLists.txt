# Micro OS — portable kernel core.
#
# The sources in this component contain no platform specific code: they can
# be compiled for the ESP32 target (as an ESP-IDF component) and for the
# host (as a plain static library used by the unit tests).

set(MICRO_OS_KERNEL_SRCS
    "src/ansi.c"
    "src/board.c"
    "src/blockdev.c"
    "src/clock.c"
    "src/boot.c"
    "src/console.c"
    "src/cpu.c"
    "src/cpu_default.c"
    "src/exec.c"
    "src/fb.c"
    "src/fb_ops.c"
    "src/blit.c"
    "src/input.c"
    "src/input_hid.c"
    "src/input_scancode.c"
    "src/input_set1.c"
    "src/kfile.c"
    "src/kmm.c"
    "src/kmod.c"
    "src/ksys.c"
    "src/kv.c"
    "src/lock.c"
    "src/log.c"
    "src/logfile.c"
    "src/mm.c"
    "src/net.c"
    "src/panic.c"
    "src/partition.c"
    "src/power.c"
    "src/port_task_default.c"
    "src/render.c"
    "src/root.c"
    "src/sched.c"
    "src/sysinfo.c"
    "src/task.c"
    "src/tty.c"
    "src/vfs.c"
    "src/version.c"
    "src/volume.c"
    "src/wait.c"
    "src/watchdog.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_KERNEL_SRCS}
        INCLUDE_DIRS "include"
    )

    # The kernel flags of the board (ADR 0010): the PSRAM is there, and the
    # machine runs a program of its own machine (ADR 0018): the MOX1 parser,
    # execve and the exec command are compiled, and the port enters an Xtensa
    # image in the windows of src/port_exec.c. PUBLIC, because the shell, the
    # library and the syscalls ask the same questions.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC KERNEL_PSRAM=1 KERNEL_EXEC=1)

    # The ring of a pipe of the board (kernel/kfile.h): 4 KiB, a sixteenth of
    # the buffer of the machine of a desk. The internal RAM of this board is
    # about a hundred kilobytes and a driver and a task live in it, so the
    # block of a pipe is paid per pipe (sixteen of them are 64 KiB of the
    # worst case, and only when sixteen are open) and the allocator falls
    # back to the PSRAM when the internal RAM is short.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KFILE_PIPE_SIZE=4096)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_kernel STATIC ${MICRO_OS_KERNEL_SRCS})
    target_include_directories(microos_kernel PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(microos_kernel PUBLIC c_std_11)
endif()
