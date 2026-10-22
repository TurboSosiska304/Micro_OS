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
        cases/test_boot.c
        cases/test_kmod.c
        cases/test_kmm.c
        cases/test_mfs.c
        cases/test_mfs_persistence.c
        cases/test_vfs.c
        cases/test_sysfs.c
        cases/test_ksys.c
        cases/test_ksys_process.c
        cases/test_libc.c
        cases/test_kshell.c
        cases/test_task.c
        cases/test_sched.c
        cases/test_sched_queue.c
        cases/test_wifi.c
    )

    target_include_directories(microos_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/framework"
    )

    target_link_libraries(microos_tests PRIVATE microos_kernel microos_board microos_kshell microos_mfs microos_procfs microos_devfs microos_libc microos_wifi microos_port_host)

# The corpus of the tests: a fixed set of numbers with the boundary values
target_compile_definitions(microos_tests PRIVATE
    KTEST_CORPUS="${CMAKE_SOURCE_DIR}/tests/corpus/vectors.txt"
)

    add_test(NAME unit COMMAND microos_tests)
endif()
