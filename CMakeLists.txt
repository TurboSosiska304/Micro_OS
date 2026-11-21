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

    # The same code runs on the board, where there is no debugger: the host
    # build is the place where a wrong pointer or undefined behaviour can be
    # seen. "make test-asan" turns the sanitizers on for the tests.
    option(MICRO_OS_SANITIZE
           "the host build with the address and the undefined behaviour sanitizers" OFF)

    if(MICRO_OS_SANITIZE)
        message(STATUS "Micro OS: sanitizers are on")
        add_compile_options(
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
        )
        add_link_options(-fsanitize=address,undefined)
    endif()

    # The race detector is a build of its own: it may not share the memory
    # of the address sanitizer, and the two answer different questions.
    # The kernel is one address space with a mutex standing in for the
    # critical section on the host (ADR 0001), so this build is the closest
    # the tests get to preemption.
    option(MICRO_OS_SANITIZE_THREAD
           "the host build with the thread sanitizer" OFF)

    if(MICRO_OS_SANITIZE_THREAD)
        message(STATUS "Micro OS: the thread sanitizer is on")
        add_compile_options(-fsanitize=thread -fno-sanitize-recover=all)
        add_link_options(-fsanitize=thread)
    endif()

    message(STATUS "Micro OS: host build (simulator + unit tests)")

    # The filesystems of the image (ADR 0009). MicroFS is the root of the
    # board, the simulator and the tests, so it cannot be left out of this
    # build; the second filesystem is a reader that an image links only when
    # it has a medium to point it at. The tests compile the FAT sources of
    # their own, so the reader is always covered.
    option(MICRO_OS_FS_MFS "MicroFS, the filesystem of the board" ON)
    option(MICRO_OS_FS_FAT32 "FAT12/16/32, read-only, for the tests and a card" OFF)
    option(MICRO_OS_FS_EXT2 "ext2, read-only (not written yet)" OFF)

    if(NOT MICRO_OS_FS_MFS)
        message(FATAL_ERROR "the host simulator and the tests mount MicroFS")
    endif()

    if(MICRO_OS_FS_EXT2)
        message(FATAL_ERROR "the ext2 reader is not written yet")
    endif()

    enable_testing()

    add_subdirectory(components/kernel)
    add_subdirectory(components/board)
    add_subdirectory(components/kshell)
    add_subdirectory(components/init)
    add_subdirectory(components/config)
    add_subdirectory(components/net)
    add_subdirectory(components/mfs)
    if(MICRO_OS_FS_FAT32)
        add_subdirectory(components/fs_fat32)
    endif()
    add_subdirectory(components/procfs)
    add_subdirectory(components/devfs)
    add_subdirectory(components/font)
    add_subdirectory(components/graphics)
    add_subdirectory(components/libc)
    add_subdirectory(components/telnet)
    add_subdirectory(components/wifi)
    add_subdirectory(ports/port_host)
    add_subdirectory(ports/port_null)
    add_subdirectory(tests)

endif()
