# Micro OS — host unit tests.
#
# Enabled only for the host build; on the ESP32 target the tests are not
# compiled into the firmware.

if(NOT ESP_PLATFORM)
    add_executable(microos_tests
        src/main.c
        framework/ktest.c
        cases/test_version.c
        cases/test_board.c
        cases/test_log.c
        cases/test_port.c
        cases/test_blockdev.c
        cases/test_render.c
        cases/test_boot.c
        cases/test_kmod.c
        cases/test_kmm.c
        cases/test_exec.c
        cases/test_fuzz.c
        cases/test_mfs.c
        cases/test_mfs_persistence.c
        cases/test_vfs.c
        cases/test_fs_fat.c
        cases/test_sysfs.c
        cases/test_input_debounce.c
        cases/test_battery_soc.c
        cases/test_fb_draw.c
        cases/test_rtc.c
        cases/test_console.c
        cases/test_telnet.c
        cases/test_text_wrap.c
        cases/test_ksys.c
        cases/test_ksys_process.c
        cases/test_libc.c
        cases/test_kshell.c
        cases/test_initd.c
        cases/test_config.c
        cases/test_net.c
        cases/test_task.c
        cases/test_sched.c
        cases/test_sched_queue.c
        cases/test_wifi.c
    )

    target_include_directories(microos_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/framework"
        # The debounce of the buttons lives with the drivers, which are
        # ESP32 only, but the file itself has no hardware in it - so the
        # tests compile it directly and replay the samples through it.
        # The parser of telnet is the same story.
        "${CMAKE_SOURCE_DIR}/components/drivers/include"
        "${CMAKE_SOURCE_DIR}/components/telnet/include"
        "${CMAKE_SOURCE_DIR}/components/graphics/include"
        "${CMAKE_SOURCE_DIR}/components/fs_fat32/include"
    )

    target_sources(microos_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/components/drivers/src/input_debounce.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/battery_soc.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/fb_draw.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/rtc_time.c"
        "${CMAKE_SOURCE_DIR}/components/telnet/src/telnet_proto.c"
        "${CMAKE_SOURCE_DIR}/components/graphics/src/text_wrap.c"
        # The FAT reader is off in the firmware by default (ADR 0009), so
        # the test binary compiles its sources of its own: the reader is
        # covered whether or not an image links it.
        "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_core.c"
        "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_dir.c"
        "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_vfs.c"
    )

    target_link_libraries(microos_tests PRIVATE microos_kernel microos_board microos_kshell microos_init microos_config microos_net microos_mfs microos_procfs microos_devfs microos_libc microos_wifi microos_port_host)

# The corpus of the tests: a fixed set of numbers with the boundary values
target_compile_definitions(microos_tests PRIVATE
    KTEST_CORPUS="${CMAKE_SOURCE_DIR}/tests/corpus/vectors.txt"
)

    # A test that hangs is a test that failed: the panic of the kernel
    # stops the process of the host, and anything else is a hang that has
    # to be reported, not waited for.
    add_test(NAME unit COMMAND microos_tests)
    set_tests_properties(unit PROPERTIES TIMEOUT 120)

    # The layout of the image that tools/mkprog.sh writes. The test builds
    # tiny objects with the compiler of the host and needs no ESP-IDF; a
    # machine without the binutils skips itself and still passes.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME mkprog
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/test_mkprog.py"
        )
        set_tests_properties(mkprog PROPERTIES TIMEOUT 60)
    endif()
endif()
