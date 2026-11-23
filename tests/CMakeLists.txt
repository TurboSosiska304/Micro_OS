# Micro OS — host unit tests.
#
# Enabled only for the host build; on the ESP32 target the tests are not
# compiled into the firmware.

if(NOT ESP_PLATFORM)
    set(MICRO_OS_TEST_CASES
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
        cases/test_lua.c
        cases/test_zstd.c
        cases/test_userland_float.c
        cases/test_file.c
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
    set(MICROOS_ZSTD_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd")
    set(MICROOS_LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")

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
        # The headers of Lua. The host build keeps the default configuration
        # of the release: the port layer of the machine
        # (`third_party/lua/port/luaconf_port.h`) is for the guest, where
        # there is no shell and no temporary name.
        "${MICROOS_LUA_DIR}"
        # The headers of zstd, and the port header of the build (the C path of
        # the Huffman decoder, no trace, no legacy frames), which is the same
        # for the two machines.
        "${MICROOS_ZSTD_DIR}/lib"
        "${MICROOS_ZSTD_DIR}/port"
    )

    # zlib is the first third-party library of the tree, and the host test is
    # its second build: the same sources with the headers of the host and the
    # port layer of `third_party/zlib/port` for the two allocation hooks. The
    # guest build (the static archive of `ports/port_x86_64`) is checked by
    # `make x86-smoke`, which runs /bin/zcat on a .gz of MicroFS.
    # Lua is the second ported library. The whole core of the release is
    # compiled for the host - the lexer, the parser, the code generator, the
    # virtual machine and the standard libraries - and the test runs chunks
    # through it. `lua.c` and `luac.c` are the two programs of the release
    # and not a part of the library.
    set(MICROOS_LUA_SOURCES
        "${MICROOS_LUA_DIR}/lapi.c"
        "${MICROOS_LUA_DIR}/lauxlib.c"
        "${MICROOS_LUA_DIR}/lbaselib.c"
        "${MICROOS_LUA_DIR}/lcode.c"
        "${MICROOS_LUA_DIR}/lcorolib.c"
        "${MICROOS_LUA_DIR}/lctype.c"
        "${MICROOS_LUA_DIR}/ldblib.c"
        "${MICROOS_LUA_DIR}/ldebug.c"
        "${MICROOS_LUA_DIR}/ldo.c"
        "${MICROOS_LUA_DIR}/ldump.c"
        "${MICROOS_LUA_DIR}/lfunc.c"
        "${MICROOS_LUA_DIR}/lgc.c"
        "${MICROOS_LUA_DIR}/linit.c"
        "${MICROOS_LUA_DIR}/liolib.c"
        "${MICROOS_LUA_DIR}/llex.c"
        "${MICROOS_LUA_DIR}/lmathlib.c"
        "${MICROOS_LUA_DIR}/lmem.c"
        "${MICROOS_LUA_DIR}/loadlib.c"
        "${MICROOS_LUA_DIR}/lobject.c"
        "${MICROOS_LUA_DIR}/lopcodes.c"
        "${MICROOS_LUA_DIR}/loslib.c"
        "${MICROOS_LUA_DIR}/lparser.c"
        "${MICROOS_LUA_DIR}/lstate.c"
        "${MICROOS_LUA_DIR}/lstring.c"
        "${MICROOS_LUA_DIR}/lstrlib.c"
        "${MICROOS_LUA_DIR}/ltable.c"
        "${MICROOS_LUA_DIR}/ltablib.c"
        "${MICROOS_LUA_DIR}/ltm.c"
        "${MICROOS_LUA_DIR}/lundump.c"
        "${MICROOS_LUA_DIR}/lutf8lib.c"
        "${MICROOS_LUA_DIR}/lvm.c"
        "${MICROOS_LUA_DIR}/lzio.c"
    )
    set_source_files_properties(${MICROOS_LUA_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter")

    # The formatter of the guest, compiled for the host into a unit of its
    # own: the release of the test binary is linked against the C library of
    # the host, and `FILE` is not the same type in the two libraries. The
    # include directories are for this file only, so the rest of the binary
    # keeps the headers of the host.
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/cases/userland_float_shim.c"
        PROPERTIES COMPILE_OPTIONS
        "-I${CMAKE_SOURCE_DIR}/components/libc/user/include;-I${CMAKE_SOURCE_DIR}/components/kernel/include")

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

    # zstd is the third library of somebody else, and its host build is the
    # same list of sources as the guest build of `ports/port_x86_64`: the
    # library of the frame format, with the port header that turns the
    # assembly of the Huffman decoder, the trace and the legacy frames off.
    set(MICROOS_ZSTD_SOURCES
        "${MICROOS_ZSTD_DIR}/lib/common/debug.c"
        "${MICROOS_ZSTD_DIR}/lib/common/entropy_common.c"
        "${MICROOS_ZSTD_DIR}/lib/common/error_private.c"
        "${MICROOS_ZSTD_DIR}/lib/common/fse_decompress.c"
        "${MICROOS_ZSTD_DIR}/lib/common/pool.c"
        "${MICROOS_ZSTD_DIR}/lib/common/threading.c"
        "${MICROOS_ZSTD_DIR}/lib/common/xxhash.c"
        "${MICROOS_ZSTD_DIR}/lib/common/zstd_common.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/fse_compress.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/hist.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/huf_compress.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_compress.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_compress_literals.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_compress_sequences.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_compress_superblock.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_double_fast.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_fast.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_lazy.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_ldm.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_opt.c"
        "${MICROOS_ZSTD_DIR}/lib/compress/zstd_preSplit.c"
        "${MICROOS_ZSTD_DIR}/lib/decompress/huf_decompress.c"
        "${MICROOS_ZSTD_DIR}/lib/decompress/zstd_ddict.c"
        "${MICROOS_ZSTD_DIR}/lib/decompress/zstd_decompress.c"
        "${MICROOS_ZSTD_DIR}/lib/decompress/zstd_decompress_block.c"
    )
    set_source_files_properties(${MICROOS_ZSTD_SOURCES}
        PROPERTIES COMPILE_OPTIONS
        "-include;${MICROOS_ZSTD_DIR}/port/zstd_port.h;-Wno-sign-compare;-Wno-unused-but-set-variable"
        OBJECT_DEPENDS "${MICROOS_ZSTD_DIR}/port/zstd_port.h")

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
        ${MICROOS_LUA_SOURCES}
        ${MICROOS_ZSTD_SOURCES}
        "${CMAKE_CURRENT_LIST_DIR}/cases/userland_float_shim.c"
    )

    # --- file and libmagic (third_party/file) -----------------------------
    #
    # The library is the same source as the guest archive, compiled for the
    # host with the port layer of the directory: the test is the second build
    # of the port and covers the values of the format, not the door of the
    # system call.
    include("${CMAKE_SOURCE_DIR}/cmake/libmagic.cmake")
    set_source_files_properties(${MICROOS_FILE_SOURCES}
        PROPERTIES COMPILE_DEFINITIONS "HAVE_CONFIG_H=1"
                   COMPILE_OPTIONS "${MICROOS_FILE_WARNINGS}")

    # The database is made by the library itself, as in the guest build: the
    # tool compiles the text the script chose, and the two levels of the port
    # are two files of the build (`magic` and `magic.mgc` beside it).
    set(MICROOS_TEST_MAGIC "${CMAKE_BINARY_DIR}/magic")
    set(MICROOS_TEST_MAGIC_MGC "${CMAKE_BINARY_DIR}/magic.mgc")
    set(MICROOS_TEST_MAGIC_TEXT "${CMAKE_BINARY_DIR}/magic-text")

    add_executable(microos_magic_compile
        ${MICROOS_FILE_SOURCES}
        "${MICROOS_FILE_DIR}/port/magic_compile.c"
    )
    target_include_directories(microos_magic_compile PRIVATE
        "${MICROOS_FILE_DIR}/src" "${MICROOS_FILE_DIR}/port")
    target_compile_definitions(microos_magic_compile PRIVATE HAVE_CONFIG_H=1)
    target_compile_options(microos_magic_compile PRIVATE ${MICROOS_FILE_WARNINGS})

    add_custom_command(
        OUTPUT "${MICROOS_TEST_MAGIC}"
        COMMAND ${CMAKE_COMMAND} -E env python3
                "${MICROOS_MAGIC_SUBSET}"
                --config "${MICROOS_MAGIC_CONFIG}"
                --out "${MICROOS_TEST_MAGIC}"
        DEPENDS "${MICROOS_MAGIC_SUBSET}" "${MICROOS_MAGIC_CONFIG}"
                "${MICROOS_FILE_DIR}/magic/microos.magic"
        COMMENT "choosing the subset of the magic database of file"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${MICROOS_TEST_MAGIC_MGC}"
        COMMAND "$<TARGET_FILE:microos_magic_compile>" -C magic
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS microos_magic_compile "${MICROOS_TEST_MAGIC}"
        COMMENT "compiling the magic database of the test"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${MICROOS_TEST_MAGIC_TEXT}"
        COMMAND ${CMAKE_COMMAND} -E copy "${MICROOS_TEST_MAGIC}" "${MICROOS_TEST_MAGIC_TEXT}"
        DEPENDS "${MICROOS_TEST_MAGIC}"
        COMMENT "the text of the magic database, without a .mgc beside it"
        VERBATIM
    )
    add_custom_target(microos_magic_database ALL
        DEPENDS "${MICROOS_TEST_MAGIC_MGC}" "${MICROOS_TEST_MAGIC_TEXT}")
    add_dependencies(microos_tests microos_magic_database)

    # The test includes the internal header of the library (file.h), which
    # wants the configuration of the port as the library sees it.
    set_source_files_properties(cases/test_file.c
        PROPERTIES COMPILE_DEFINITIONS "HAVE_CONFIG_H=1")
    target_sources(microos_tests PRIVATE ${MICROOS_FILE_SOURCES})
    target_include_directories(microos_tests PRIVATE
        "${MICROOS_FILE_DIR}/src" "${MICROOS_FILE_DIR}/port")
    target_compile_definitions(microos_tests PRIVATE
        MICROOS_TEST_MAGIC="${MICROOS_TEST_MAGIC}"
        MICROOS_TEST_MAGIC_TEXT="${MICROOS_TEST_MAGIC_TEXT}")

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

    # The math of the host: the core of Lua calls the functions of the C
    # library of the release (pow, fmod, floor, ...) and the host keeps them
    # in libm, which the linker does not take by itself.
    target_link_libraries(microos_tests PRIVATE ${MICRO_OS_TEST_LIBS} m)

    # The race detector of this toolchain crashes on any `longjmp` after a
    # `setjmp` ("can't find longjmp buf"), and a ten-line C program reproduces
    # it: the library of the runner and the C library of the machine do not
    # agree about the buffer. The error path of Lua is a long jump, so the one
    # case that takes it is left out of the build under the detector; the
    # other cases of the port and every case of the kernel still run.
    if(MICRO_OS_SANITIZE_THREAD)
        target_compile_definitions(microos_tests PRIVATE MICROOS_TSAN=1)
    endif()

# The corpus of the tests: a fixed set of numbers with the boundary values,
# the recorded ABI of the release (tests/corpus/abi.txt) and the MOX1 image
# that 1.0 froze (tests/corpus/mox1).
target_compile_definitions(microos_tests PRIVATE
    KTEST_CORPUS="${CMAKE_SOURCE_DIR}/tests/corpus/vectors.txt"
    KTEST_ABI_GOLDEN="${CMAKE_SOURCE_DIR}/tests/corpus/abi.txt"
    KTEST_MOX_FIXTURE="${CMAKE_SOURCE_DIR}/tests/corpus/mox1/frozen-x86_64.hex"
)

    # --- the library of a program, the pure part --------------------------
    #
    # The functions of `components/libc/user` that have no door under them
    # (the classes, the strings, the pattern of a name, the options, the
    # regular expressions, the one user and the decimal order of sbase) are
    # compiled for the host with the headers of a program. The binary is a
    # test of its own: the include path of a program would shadow <stdio.h>
    # and <string.h> of the other tests.
    set(MICROOS_USERLAND_TESTS
        cases/test_userland.c
        src/userland_main.c
        framework/ktest.c
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/ctype.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/string_ext.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/fnmatch.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/getopt.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/regex.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/libgen.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/pwd.c"
        "${CMAKE_SOURCE_DIR}/components/libc/user/src/errno.c"
        "${CMAKE_SOURCE_DIR}/third_party/sbase/port/numcmp.c"
    )

    add_executable(microos_userland_tests ${MICROOS_USERLAND_TESTS})
    target_include_directories(microos_userland_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/framework"
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_SOURCE_DIR}/components/libc/user/include"
        "${CMAKE_SOURCE_DIR}/components/kernel/include"
        "${CMAKE_SOURCE_DIR}/third_party/sbase/port"
    )
    target_compile_options(microos_userland_tests PRIVATE -Wall -Wextra -Werror -Wno-nonnull-compare)
    add_test(NAME userland COMMAND microos_userland_tests)
    set_tests_properties(userland PROPERTIES TIMEOUT 60)

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
