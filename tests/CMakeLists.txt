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
        cases/test_task.c
        cases/test_sched.c
    )

    target_include_directories(microos_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/framework"
    )

    target_link_libraries(microos_tests PRIVATE microos_kernel microos_board microos_port_host)

    add_test(NAME unit COMMAND microos_tests)
endif()
