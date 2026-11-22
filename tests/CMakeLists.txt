# Micro OS — host unit tests.
#
# Enabled only for the host build; on the ESP32 target the tests are not
# compiled into the firmware.

if(NOT ESP_PLATFORM)
    set(MICRO_OS_TEST_CASES
        cases/test_version.c
        cases/test_board.c
        cases/test_log.c
        cases/test_ansi.c
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
        cases/test_mfs_repair.c
        cases/test_mfs_many.c
        cases/test_mkfs_image.c
        cases/test_vfs.c
        cases/test_fs_fat.c
        cases/test_fs_ext2.c
        cases/test_sysfs.c
        cases/test_input_debounce.c
        cases/test_battery_soc.c
        cases/test_fb_draw.c
        cases/test_rtc.c
        cases/test_ir_nec.c
        cases/test_sound_detect.c
        cases/test_console.c
        cases/test_telnet.c
        cases/test_text_wrap.c
        cases/test_ksys.c
        cases/test_ksys_process.c
        cases/test_libc.c
        cases/test_kshell.c
        cases/test_kshell_pipe.c
        cases/test_initd.c
        cases/test_config.c
        cases/test_settings.c
        cases/test_task.c
        cases/test_sched.c
        cases/test_sched_queue.c
        cases/test_smp.c
        cases/test_sign.c
        cases/test_progs.c
        cases/test_zlib.c
    )

    # The tests of a component that a build leaves out are left out with it:
    # they name the sources and the commands that are not in the image
    # (ADR 0010, make test-minimal). The test of the network covers every
    # protocol at once, so it needs the whole component: a build that turns
    # one client off keeps the component and loses that case, not the build.
    if(MICRO_OS_NET AND MICRO_OS_NET_HTTP AND MICRO_OS_NET_HTTP_SERVER
       AND MICRO_OS_NET_NTP AND MICRO_OS_NET_MQTT)
        list(APPEND MICRO_OS_TEST_CASES cases/test_net.c)
    endif()
    if(MICRO_OS_NET AND MICRO_OS_NET_HTTP)
        list(APPEND MICRO_OS_TEST_CASES cases/test_pkg.c)
    endif()
    if(MICRO_OS_WIFI)
        list(APPEND MICRO_OS_TEST_CASES cases/test_wifi.c)
    endif()

    add_executable(microos_tests
        src/main.c
        framework/ktest.c
        ${MICRO_OS_TEST_CASES}
    )

    set(MICROOS_ZLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")

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
        "${CMAKE_SOURCE_DIR}/components/fs_ext2/include"
        # The verifier of ADR 0020 and its two primitives: the boot loader and
        # the tool compile the same files, and the test is the third caller.
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader"
        # The directory of the `progs` partition (ADR 0022): read by the port
        # of the board and by the test, written by tools/progs_image.py.
        "${CMAKE_SOURCE_DIR}/components/port_esp32/include"
        # zlib and the headers of its port (the two allocation hooks).
        "${MICROOS_ZLIB_DIR}"
        "${MICROOS_ZLIB_DIR}/port"
    )

    # zlib is the first third-party library of the tree, and the host test is
    # its second build: the same sources with the headers of the host and the
    # port layer of `third_party/zlib/port` for the two allocation hooks. The
    # guest build (the static archive of `ports/port_x86_64`) is checked by
    # `make x86-smoke`, which runs /bin/zcat on a .gz of MicroFS.
    set(MICROOS_ZLIB_SOURCES
        "${MICROOS_ZLIB_DIR}/adler32.c"
        "${MICROOS_ZLIB_DIR}/compress.c"
        "${MICROOS_ZLIB_DIR}/crc32.c"
        "${MICROOS_ZLIB_DIR}/deflate.c"
        "${MICROOS_ZLIB_DIR}/gzclose.c"
        "${MICROOS_ZLIB_DIR}/gzlib.c"
        "${MICROOS_ZLIB_DIR}/gzread.c"
        "${MICROOS_ZLIB_DIR}/gzwrite.c"
        "${MICROOS_ZLIB_DIR}/infback.c"
        "${MICROOS_ZLIB_DIR}/inffast.c"
        "${MICROOS_ZLIB_DIR}/inflate.c"
        "${MICROOS_ZLIB_DIR}/inftrees.c"
        "${MICROOS_ZLIB_DIR}/trees.c"
        "${MICROOS_ZLIB_DIR}/uncompr.c"
        "${MICROOS_ZLIB_DIR}/zutil.c"
    )
    set_source_files_properties(${MICROOS_ZLIB_SOURCES}
        PROPERTIES COMPILE_DEFINITIONS "MY_ZCALLOC=1;HAVE_UNISTD_H=1;HAVE_STDARG_H=1"
                   COMPILE_OPTIONS "-Wno-sign-compare")
    set_source_files_properties("${MICROOS_ZLIB_DIR}/port/zlib_os.c"
        PROPERTIES COMPILE_DEFINITIONS "MY_ZCALLOC=1")

    target_sources(microos_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/components/drivers/src/input_debounce.c"        "${CMAKE_SOURCE_DIR}/components/drivers/src/battery_soc.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/fb_draw.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/rtc_time.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/ir_nec.c"
        "${CMAKE_SOURCE_DIR}/components/drivers/src/sound_detect.c"
        "${CMAKE_SOURCE_DIR}/components/telnet/src/telnet_proto.c"
        "${CMAKE_SOURCE_DIR}/components/graphics/src/text_wrap.c"
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader/sha256.c"
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader/ed25519.c"
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader/sign_image.c"
        "${CMAKE_SOURCE_DIR}/components/port_esp32/src/progs_format.c"
        ${MICROOS_ZLIB_SOURCES}
        "${MICROOS_ZLIB_DIR}/port/zlib_os.c"
    )

    # The FAT reader comes from the component when the host build links it
    # (the default, ADR 0009). A build that turns it off still compiles the
    # sources into the test binary: the tests are the place where the reader
    # is covered, whatever an image links.
    if(NOT MICRO_OS_FS_FAT32)
        target_sources(microos_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_core.c"
            "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_dir.c"
            "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_check.c"
            "${CMAKE_SOURCE_DIR}/components/fs_fat32/src/fat_vfs.c"
        )
    endif()

    # The ext2 reader is a reader and never a writer: the host build links it
    # by default, and a build that turns the option off (make test-minimal)
    # still compiles the sources into the test binary, so the reader is
    # covered whatever an image links (ADR 0019).
    if(NOT MICRO_OS_FS_EXT2)
        target_sources(microos_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/components/fs_ext2/src/ext2_core.c"
            "${CMAKE_SOURCE_DIR}/components/fs_ext2/src/ext2_dir.c"
            "${CMAKE_SOURCE_DIR}/components/fs_ext2/src/ext2_vfs.c"
        )
    endif()

    set(MICRO_OS_TEST_LIBS
        mkfs_mfs_image
        microos_kernel
        microos_board
        microos_kshell
        microos_init
        microos_config
        microos_settings
        microos_mfs
        microos_procfs
        microos_devfs
        microos_libc
        microos_port_host
    )

    if(MICRO_OS_NET)
        list(APPEND MICRO_OS_TEST_LIBS microos_net)
    endif()
    if(MICRO_OS_NET AND MICRO_OS_NET_HTTP)
        list(APPEND MICRO_OS_TEST_LIBS microos_pkg)
    endif()
    if(MICRO_OS_WIFI)
        list(APPEND MICRO_OS_TEST_LIBS microos_wifi)
    endif()
    if(MICRO_OS_FS_EXT2)
        list(APPEND MICRO_OS_TEST_LIBS microos_fs_ext2)
    endif()

    target_link_libraries(microos_tests PRIVATE ${MICRO_OS_TEST_LIBS})

# The corpus of the tests: a fixed set of numbers with the boundary values,
# the recorded ABI of the release (tests/corpus/abi.txt) and the MOX1 image
# that 1.0 froze (tests/corpus/mox1).
target_compile_definitions(microos_tests PRIVATE
    KTEST_CORPUS="${CMAKE_SOURCE_DIR}/tests/corpus/vectors.txt"
    KTEST_ABI_GOLDEN="${CMAKE_SOURCE_DIR}/tests/corpus/abi.txt"
    KTEST_MOX_FIXTURE="${CMAKE_SOURCE_DIR}/tests/corpus/mox1/frozen-x86_64.hex"
)

    # A test that hangs is a test that failed: the panic of the kernel
    # stops the process of the host, and anything else is a hang that has
    # to be reported, not waited for.
    add_test(NAME unit COMMAND microos_tests)
    set_tests_properties(unit PROPERTIES TIMEOUT 120)

    # The kernel of two cores (ADR 0012): the host port takes the number of
    # cores of the run from the environment, exactly as it does for the
    # simulator and the soak - so the build with KERNEL_SMP on runs the tests
    # on two cores without a second binary. One core is the default of the
    # plain build, and its tests are the tests of 1.0.
    if(KERNEL_SMP)
        set_tests_properties(unit PROPERTIES ENVIRONMENT "MICRO_OS_CPUS=2")
    endif()

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
