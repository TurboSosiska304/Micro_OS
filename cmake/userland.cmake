# Micro OS — the recipe of a program and of a ported library.
#
# A program of the x86-64 port is not a CMake executable of the host: it is
# linked with -nostdlib for the window of the machine, against the library of
# `components/libc/user`, and turned into a MOX1 image by `tools/mkprog.sh`.
# The same recipe serves a program of the tree and a program that was ported
# from somebody else (zlib today, Lua and the next one tomorrow), so the
# steps live here and not in the file of one program.
#
# The caller names the flags and the paths, because they are the contract of
# its machine, and this module names only the steps:
#
#   MICROOS_USERLAND_CFLAGS   the flags of a guest object (no -c, no -o)
#   MICROOS_PROGRAM_CFLAGS    the same plus -c, for one file of a program
#   MICROOS_KERNEL_INCLUDE    the headers of the ABI (kernel/ksys.h)
#   MICROOS_LIBC_USER_DIR     components/libc/user
#   MICROOS_PROGRAM_DIR       the directory of the program.ld and prog.h
#   MICROOS_PROGRAM_BASE      the base of the window of the port
#   MICROOS_MKPROG            tools/mkprog.sh
#
# A ported library is a static archive of the objects of the guest, built by
# the same compiler with the same flags. It is linked into the program and
# nothing of the host libc enters the image: the archive may reference only
# the calls of `components/libc/user` and the door of the port.

# The runtime of every program: the entry, the door, the formatter, the
# strings, the streams and the arena. A program carries its own copy, which
# is what "statically linked" means on this machine.
set(MICROOS_LIBC_USER_SOURCES
    "${MICROOS_LIBC_USER_DIR}/src/start.c"
    "${MICROOS_LIBC_USER_DIR}/src/syscall.c"
    "${MICROOS_LIBC_USER_DIR}/src/stdio.c"
    "${MICROOS_LIBC_USER_DIR}/src/string.c"
    "${MICROOS_LIBC_USER_DIR}/src/errno.c"
)

# The headers the runtime and the program include. A custom command does not
# get a depfile from CMake, so a change to one of them is named here: without
# it a rebuild after an edit of `ulibc.h` would keep the old object.
set(MICROOS_LIBC_USER_HEADERS
    "${MICROOS_LIBC_USER_DIR}/include/ulibc.h"
    "${MICROOS_LIBC_USER_DIR}/include/assert.h"
    "${MICROOS_LIBC_USER_DIR}/include/ctype.h"
    "${MICROOS_LIBC_USER_DIR}/include/errno.h"
    "${MICROOS_LIBC_USER_DIR}/include/fcntl.h"
    "${MICROOS_LIBC_USER_DIR}/include/locale.h"
    "${MICROOS_LIBC_USER_DIR}/include/math.h"
    "${MICROOS_LIBC_USER_DIR}/include/setjmp.h"
    "${MICROOS_LIBC_USER_DIR}/include/signal.h"
    "${MICROOS_LIBC_USER_DIR}/include/stdio.h"
    "${MICROOS_LIBC_USER_DIR}/include/stdlib.h"
    "${MICROOS_LIBC_USER_DIR}/include/string.h"
    "${MICROOS_LIBC_USER_DIR}/include/time.h"
    "${MICROOS_LIBC_USER_DIR}/include/unistd.h"
    "${MICROOS_LIBC_USER_DIR}/include/sys/types.h"
    # The two headers of the library that are not a part of its interface but
    # are included by a source of it: the state of a stream and the seam of
    # the formatter with the number with a point. They are named for the same
    # reason as the others: a custom command gets no depfile from CMake.
    "${MICROOS_LIBC_USER_DIR}/src/file_private.h"
    "${MICROOS_LIBC_USER_DIR}/src/format_float.h"
)

# A static library of the guest. The sources are the ones of the third-party
# directory; INCLUDES and DEFINES are the port layer of that directory. The
# archive is not linked by CMake into anything: it is a file that a program
# of the port names in its own link, so the host libc of the build machine is
# never asked for a symbol.
function(microos_userland_library name)
    cmake_parse_arguments(LIB "FLOAT" "" "SOURCES;INCLUDES;DEFINES;COMPILE_OPTIONS" ${ARGN})

    if(NOT LIB_SOURCES)
        message(FATAL_ERROR "microos_userland_library(${name}): no sources")
    endif()

    if(LIB_FLOAT)
        set(guest_flags ${MICROOS_FLOAT_CFLAGS})
    else()
        set(guest_flags ${MICROOS_USERLAND_CFLAGS})
    endif()

    add_library(${name} STATIC ${LIB_SOURCES})
    target_compile_options(${name} PRIVATE ${guest_flags} ${LIB_COMPILE_OPTIONS})
    target_include_directories(${name} PRIVATE
        "${MICROOS_KERNEL_INCLUDE}"
        "${MICROOS_LIBC_USER_DIR}/include"
        ${LIB_INCLUDES}
    )
    target_compile_definitions(${name} PRIVATE ${LIB_DEFINES})
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE OFF
    )
endfunction()

# A program of the port: the runtime, the file of the program, the libraries
# it names, the script of the window and the image of the format. The entry
# is `_start` of the runtime (ADR 0016), so `main` is a function of the
# program and not the place the port enters.
#
# A program whose text is near the limit of the image is built with
# `GC_SECTIONS`: every function and every object is a section of its own, and
# the linker of the window drops the ones no call reaches. It is the size flag
# of a ported library whose engine is bigger than the program around it.
function(microos_add_userland_program name)
    cmake_parse_arguments(PROG "FLOAT;GC_SECTIONS" "MAIN"
        "LIBRARIES;INCLUDES;DEFINES;SOURCES;COMPILE_OPTIONS" ${ARGN})

    if(PROG_MAIN)
        set(source "${PROG_MAIN}")
    else()
        set(source "${MICROOS_PROGRAM_DIR}/${name}.c")
    endif()
    set(image "${CMAKE_BINARY_DIR}/${name}.mox")
    set(objects)
    set(runtime_sources ${MICROOS_LIBC_USER_SOURCES} ${PROG_SOURCES})
    if(PROG_FLOAT)
        set(compile_flags ${MICROOS_FLOAT_PROGRAM_CFLAGS} ${PROG_COMPILE_OPTIONS})
    else()
        set(compile_flags ${MICROOS_PROGRAM_CFLAGS} ${PROG_COMPILE_OPTIONS})
    endif()
    set(link_flags)
    if(PROG_GC_SECTIONS)
        list(APPEND compile_flags -ffunction-sections -fdata-sections)
        list(APPEND link_flags -Wl,--gc-sections)
    endif()
    set(include_flags
        -I "${MICROOS_KERNEL_INCLUDE}"
        -I "${MICROOS_PROGRAM_DIR}"
        -I "${MICROOS_LIBC_USER_DIR}/include"
    )
    foreach(directory ${PROG_INCLUDES})
        list(APPEND include_flags -I "${directory}")
    endforeach()
    foreach(definition ${PROG_DEFINES})
        list(APPEND compile_flags -D ${definition})
    endforeach()

    set(dependencies
        "${source}"
        "${MICROOS_PROGRAM_DIR}/program.ld"
        "${MICROOS_PROGRAM_DIR}/prog.h"
        "${MICROOS_KERNEL_INCLUDE}/kernel/ksys.h"
        "${MICROOS_MKPROG}"
        ${runtime_sources}
        ${MICROOS_LIBC_USER_HEADERS}
    )

    foreach(runtime ${runtime_sources})
        get_filename_component(stem "${runtime}" NAME_WE)
        set(object "${CMAKE_BINARY_DIR}/${name}_${stem}.o")
        add_custom_command(
            OUTPUT "${object}"
            COMMAND ${CMAKE_C_COMPILER} ${compile_flags} ${include_flags}
                    -o "${object}" "${runtime}"
            DEPENDS "${runtime}" ${MICROOS_LIBC_USER_HEADERS}
            COMMENT "compiling ${stem} for /bin/${name}"
            VERBATIM
        )
        list(APPEND objects "${object}")
    endforeach()

    set(program_object "${CMAKE_BINARY_DIR}/${name}.o")
    add_custom_command(
        OUTPUT "${program_object}"
        COMMAND ${CMAKE_C_COMPILER} ${compile_flags} ${include_flags}
                -o "${program_object}" "${source}"
        DEPENDS "${source}" "${MICROOS_PROGRAM_DIR}/prog.h"
                "${MICROOS_KERNEL_INCLUDE}/kernel/ksys.h"
                ${MICROOS_LIBC_USER_HEADERS}
        COMMENT "compiling /bin/${name}"
        VERBATIM
    )
    list(APPEND objects "${program_object}")

    set(archives)
    foreach(library ${PROG_LIBRARIES})
        list(APPEND archives "$<TARGET_FILE:${library}>")
        list(APPEND dependencies ${library})
    endforeach()

    add_custom_command(
        OUTPUT "${image}"
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie -Wl,--build-id=none
                ${link_flags} -Wl,-e,_start
                -T "${MICROOS_PROGRAM_DIR}/program.ld" -o "${name}.elf"
                ${objects} ${archives}
        COMMAND sh "${MICROOS_MKPROG}" --base ${MICROOS_PROGRAM_BASE} --arch x86-64
                "${name}.elf" "${image}" _start
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS ${objects} ${dependencies} ${archives}
        COMMENT "linking the image of /bin/${name}"
        VERBATIM
    )

    set(MICROOS_PROGRAM_IMAGES ${MICROOS_PROGRAM_IMAGES} "${image}" PARENT_SCOPE)
endfunction()
