# Micro OS — top level build file.
#
# The same sources are built in two ways:
#
#   ESP32 (device)  : via ESP-IDF   ->  idf.py build / idf.py flash
#   HOST (simulator): via plain CMake ->  cmake -B build-host && ctest
#
# Detection is automatic: if IDF_PATH points to a valid ESP-IDF checkout the
# firmware build is used, otherwise the host build is configured.

cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    set(MICRO_OS_PLATFORM "ESP32")
else()
    set(MICRO_OS_PLATFORM "HOST")
endif()

if(MICRO_OS_PLATFORM STREQUAL "ESP32")

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(micro_os C)

    message(STATUS "Micro OS: ESP32 firmware build (ESP-IDF ${IDF_VERSION})")

else()

    project(micro_os_host C)

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS ON)

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
    endif()

    add_compile_options(
        -Wall
        -Wextra
        -Werror
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wpointer-arith
        -Wundef
        -Wno-unused-parameter
    )

    message(STATUS "Micro OS: host build (simulator + unit tests)")

    enable_testing()

    add_subdirectory(components/kernel)
    add_subdirectory(components/board)
    add_subdirectory(components/kshell)
    add_subdirectory(components/mfs)
    add_subdirectory(components/procfs)
    add_subdirectory(components/devfs)
    add_subdirectory(components/font)
    add_subdirectory(ports/port_host)
    add_subdirectory(tests)

endif()
