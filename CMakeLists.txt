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

    # The same moment and the same paths on every build (SOURCE_DATE_EPOCH):
    # the firmware image is then the same bytes for the same inputs.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/reproducible.cmake")

    # The version of the application, as the kernel says it. esp_app_desc_t
    # carries it, and the anti-rollback of the board compares it with the
    # version in the NVS (ADR 0020); it should be the same number the bundle
    # and `uname` print.
    file(READ "${CMAKE_CURRENT_LIST_DIR}/components/kernel/include/kernel/config.h" _version_header)
    string(REGEX MATCH "#define KERNEL_VERSION_MAJOR ([0-9]+)" _major "${_version_header}")
    set(_major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define KERNEL_VERSION_MINOR ([0-9]+)" _minor "${_version_header}")
    set(_minor "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define KERNEL_VERSION_PATCH ([0-9]+)" _patch "${_version_header}")
    set(_patch "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define KERNEL_VERSION_SUFFIX \"([^\"]*)\"" _suffix "${_version_header}")
    set(_suffix "${CMAKE_MATCH_1}")
    set(PROJECT_VER "${_major}.${_minor}.${_patch}${_suffix}")

    # The private key of the image (ADR 0020) is a path outside this tree; a
    # build without it uses the development key in its own directory. The
    # release workflow puts the material of the secret in a file and exports
    # the name of that file as MICROOS_SIGN_KEY.
    if(DEFINED ENV{MICROOS_SIGN_KEY} AND NOT "$ENV{MICROOS_SIGN_KEY}" STREQUAL "")
        set(MICROOS_SIGN_KEY "$ENV{MICROOS_SIGN_KEY}" CACHE FILEPATH
            "the private key of the image (default: a key generated in the build directory)" FORCE)
    endif()

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(micro_os C)

    # The application and the 4 KiB of its trailer have to fit the factory
    # partition: the flash layout is a file of the tree, and the check reads
    # it instead of naming the 2 MB a second time.
    set(MICRO_OS_FACTORY_SIZE 0x200000)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/partitions.csv" _partition_lines)
    foreach(_line IN LISTS _partition_lines)
        if(_line MATCHES "^factory[ \t]*,")
            string(REPLACE "," ";" _fields "${_line}")
            list(GET _fields 4 _factory_size)
            string(STRIP "${_factory_size}" _factory_size)
            if(_factory_size MATCHES "^0[xX][0-9a-fA-F]+$")
                set(MICRO_OS_FACTORY_SIZE "${_factory_size}")
            endif()
        endif()
    endforeach()
    math(EXPR MICRO_OS_FACTORY_BYTES "${MICRO_OS_FACTORY_SIZE}")

    add_custom_target(micro_os_image_fits ALL
        COMMAND ${CMAKE_COMMAND}
                -DIMAGE=${CMAKE_BINARY_DIR}/micro_os.bin
                -DPARTITION_SIZE=${MICRO_OS_FACTORY_BYTES}
                -DTRAILER_RESERVE=4096
                -P "${CMAKE_CURRENT_LIST_DIR}/tools/check_image_fits.cmake"
        DEPENDS gen_project_binary
        COMMENT "checking that the image and its trailer fit the factory partition"
        VERBATIM
    )

    message(STATUS "Micro OS: ESP32 firmware build (ESP-IDF ${IDF_VERSION}), version ${PROJECT_VER}")

else()

    project(micro_os_host C)

    include("${CMAKE_CURRENT_LIST_DIR}/cmake/reproducible.cmake")

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

    # The coverage build: gcov counts the lines the tests reached. It is a
    # configuration of its own (make coverage), not a part of the plain
    # build, because the counters make the binary slower and the files of
    # the build directory are not the ones of a run.
    option(MICRO_OS_COVERAGE "the host build instrumented for gcov" OFF)

    if(MICRO_OS_COVERAGE)
        message(STATUS "Micro OS: coverage is on")
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    endif()

    message(STATUS "Micro OS: host build (simulator + unit tests)")

    # The filesystems of the image (ADR 0009). MicroFS is the root of the
    # board, the simulator and the tests, so it cannot be left out of this
    # build; the second filesystem is a reader that an image links only when
    # it has a medium to point it at. The host build has one (the tests and
    # the simulator check a volume), and a build that turns the option off
    # still compiles the sources into the tests, so the reader is covered
    # either way.
    option(MICRO_OS_FS_MFS "MicroFS, the filesystem of the board" ON)
    option(MICRO_OS_FS_FAT32 "FAT12/16/32, read and written, for the tests and a card" ON)
    option(MICRO_OS_FS_EXT2 "ext2, read-only, for the tests and a disk of the world" ON)

    if(NOT MICRO_OS_FS_MFS)
        message(FATAL_ERROR "the host simulator and the tests mount MicroFS")
    endif()

    # The optional features of the image (ADR 0010). The host turns every
    # one of them on, because the tests are the place where each path is
    # compiled; a build of its own (make test-minimal) turns them off and
    # proves that the sources still build and the tests that do not need
    # the feature still pass.
    option(KERNEL_PSRAM "the PSRAM pool and its statistics in kmm" ON)
    option(KERNEL_EXEC "the MOX1 parser, execve and the exec command" ON)
    option(MICRO_OS_NET "the network services of the component net" ON)
    option(MICRO_OS_NET_HTTP "the HTTP client of the component net" ON)
    option(MICRO_OS_NET_HTTP_SERVER "the page of the state (httpd)" ON)
    option(MICRO_OS_NET_NTP "the clock from NTP" ON)
    option(MICRO_OS_NET_MQTT "the MQTT client and the mqtt command" ON)
    option(MICRO_OS_TELNET "the shell on TCP port 23" ON)
    option(MICRO_OS_WIFI "the WiFi station" ON)

    # The C sources see the numbers, not the words "ON"/"OFF": every flag is
    # `#if` in the code, and -Wundef makes an undefined one a build error.
    add_compile_definitions(
        KERNEL_PSRAM=$<BOOL:${KERNEL_PSRAM}>
        KERNEL_EXEC=$<BOOL:${KERNEL_EXEC}>
        MICRO_OS_NET=$<BOOL:${MICRO_OS_NET}>
        MICRO_OS_NET_HTTP=$<BOOL:${MICRO_OS_NET_HTTP}>
        MICRO_OS_NET_HTTP_SERVER=$<BOOL:${MICRO_OS_NET_HTTP_SERVER}>
        MICRO_OS_NET_NTP=$<BOOL:${MICRO_OS_NET_NTP}>
        MICRO_OS_NET_MQTT=$<BOOL:${MICRO_OS_NET_MQTT}>
        MICRO_OS_TELNET=$<BOOL:${MICRO_OS_TELNET}>
        MICRO_OS_WIFI=$<BOOL:${MICRO_OS_WIFI}>
        MICRO_OS_FS_FAT32=$<BOOL:${MICRO_OS_FS_FAT32}>
        MICRO_OS_FS_EXT2=$<BOOL:${MICRO_OS_FS_EXT2}>
    )

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
    if(MICRO_OS_FS_EXT2)
        add_subdirectory(components/fs_ext2)
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
    add_subdirectory(tools)
    add_subdirectory(tests)

endif()
