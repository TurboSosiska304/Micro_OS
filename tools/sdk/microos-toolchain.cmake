# Micro OS — the CMake toolchain of a program built with the SDK.
#
#     cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/microos-toolchain.cmake
#     cmake --build build
#
# The result is one file, `build/<name>.mox`, an image of the MOX1 format for
# the x86-64 port of the machine (ADR 0006, ADR 0016): the text linked at the
# base of the window of the port plus the 32-byte header, the initialised data
# after it, and nothing of the host libc in it. `add_executable(name ...)` is
# therefore the whole recipe of a program: the compile flags below, the crt,
# the library of the SDK and the link of the format.
#
# This file is not a cross compiler: the compiler is the host one (gcc or
# clang of x86-64 Linux), and the target is the freestanding program of the
# machine. `CMAKE_SYSTEM_NAME Generic` is how CMake is told that the target is
# not the host, `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` stops it from
# trying to link and run a test program, and `CMAKE_EXECUTABLE_SUFFIX .mox` is
# the name of the file the link writes.
#
# --- the compile flags
#
# They are the flags of a guest object in the tree (ports/port_x86_64/
# CMakeLists.txt), and each one is a fact of the machine:
#
#   -ffreestanding        no assumption of a host libc; `main` is a function
#   -nostdinc             the headers of the host are not the headers here;
#                         <sdk>/include and the compiler's own directory are
#   -march=x86-64         the oldest model of the machine (the kernel's rule)
#   -mno-red-zone         the port enters a program without a red zone
#   -fno-pic -fno-pie     the image is linked at an absolute window
#   -fno-stack-protector  there is no stack guard of a thread in the ABI
#   -fno-asynchronous-unwind-tables, -fno-merge-constants
#                         no note, no merged string: the image is sections
#   -ffunction-sections, -fdata-sections
#                         one section per function and object, so that
#                         -Wl,--gc-sections drops what no call reaches
#   -Os                   the size of the disk image
#
# MICROOS_FLOAT=ON is for a program whose own numbers are doubles (Lua, awk,
# SQLite): the port turns the XMM and x87 units on for ring 3, and its
# scheduler saves neither, so one such program runs at a time and `execve`
# waits for it (docs/debts.md). Without it the program is compiled with
# -mgeneral-regs-only and a double in the text is a compile error, which is
# the honest answer of a machine that cannot save the registers.
#
# The library of the SDK (`lib/libmicroos_user.a`) is one archive with the
# float path in it: a program compiled with -mgeneral-regs-only cannot reach
# it, and the code of `%f` stays out of the image because no call names it.
#
# MICROOS_WARNINGS=OFF is for the sources of somebody else, which answer the
# warnings their release answers; the default is -Wall -Wextra.
#
# --- the link
#
# A program of this machine is not linked by the default rule of CMake: the
# image has to land at the window of the port, the linker of another version
# may move the text, and the sections are turned into the header of the format
# by bin/mkprog.sh. `CMAKE_C_LINK_EXECUTABLE` therefore runs bin/link_program.sh
# (which links, measures the address and links again, then calls mkprog.sh)
# with lib/program.ld, the entry `_start` of lib/crt0.o, and the objects of
# the target followed by the archive of the library.
#
# MICROOS_GC_SECTIONS=OFF keeps every function of the archive the program
# named, which is what a debugger of a size problem wants.

get_filename_component(MICROOS_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The program is a file, not a process of the host: CMake must not try to run
# what it builds, and the file it builds is a `.mox` (share/microos-project.cmake).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_PROJECT_INCLUDE "${MICROOS_SDK_ROOT}/share/microos-project.cmake")

if(NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES cc gcc clang)
    if(NOT CMAKE_C_COMPILER)
        message(FATAL_ERROR "the SDK needs a C compiler of the host: cc, gcc or clang")
    endif()
endif()

# The base of the window of the x86-64 port (ports/port_x86_64/CMakeLists.txt,
# PROGRAM_BASE; 96 MiB at 256 MiB since ADR 0057). mkprog.sh refuses an image
# linked for another one, so a port with another window is this variable and
# nothing else: a program of the SDK linked at the old base runs its first
# instructions at the new one and names data that is not mapped.
set(MICROOS_PROGRAM_BASE "0x10000000" CACHE STRING
    "the base of the window of the program (the port's PROGRAM_BASE)")
option(MICROOS_FLOAT "the program computes in double: SSE and x87 are allowed" OFF)
option(MICROOS_GC_SECTIONS "drop the sections no call of the program reaches" ON)
option(MICROOS_WARNINGS "compile with -Wall -Wextra" ON)

# The headers of a program are the SDK's and the compiler's own (stdarg.h,
# stddef.h and the rest of the files of the language): -nostdinc takes the
# directories of the host away, and the one directory that comes back is
# where the driver says its headers are.
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-file-name=include
    OUTPUT_VARIABLE MICROOS_COMPILER_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT MICROOS_COMPILER_INCLUDE OR NOT IS_DIRECTORY "${MICROOS_COMPILER_INCLUDE}")
    message(FATAL_ERROR
        "${CMAKE_C_COMPILER} -print-file-name=include names no directory of headers")
endif()

set(MICROOS_C_FLAGS
    -ffreestanding
    -nostdinc
    -isystem "${MICROOS_SDK_ROOT}/include"
    -isystem "${MICROOS_COMPILER_INCLUDE}"
    -march=x86-64
    -mno-red-zone
    -fno-pic
    -fno-pie
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-merge-constants
    -ffunction-sections
    -fdata-sections
    -Os
)
string(REPLACE ";" " " MICROOS_C_FLAGS "${MICROOS_C_FLAGS}")
set(CMAKE_C_FLAGS_INIT "${MICROOS_C_FLAGS}")

# The flags of an option are not in CMAKE_C_FLAGS_INIT: that variable fills a
# cache entry once, and a person who turns MICROOS_FLOAT on in a directory
# that was configured without it would build with the flags of the first
# configure - the worst kind of answer, because the build keeps working and
# says "SSE disabled" about a program that asked for it. They are a part of
# the rule of a compile instead, which CMake reads on every configure.
set(MICROOS_OPTION_C_FLAGS)
if(MICROOS_WARNINGS)
    list(APPEND MICROOS_OPTION_C_FLAGS -Wall -Wextra)
endif()
if(NOT MICROOS_FLOAT)
    list(APPEND MICROOS_OPTION_C_FLAGS -mgeneral-regs-only)
endif()
string(REPLACE ";" " " MICROOS_OPTION_C_FLAGS "${MICROOS_OPTION_C_FLAGS}")
set(CMAKE_C_COMPILE_OBJECT
    "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> ${MICROOS_OPTION_C_FLAGS} \
<FLAGS> -o <OBJECT> -c <SOURCE>")

if(MICROOS_GC_SECTIONS)
    set(MICROOS_GC_LINK_FLAGS "-Wl,--gc-sections")
else()
    set(MICROOS_GC_LINK_FLAGS "")
endif()

# The link of the format: the crt, the objects of the target, the libraries it
# names and the archive of the SDK, in that order. link_program.sh owns
# -nostdlib -static -no-pie and the flags of the window; the two files after
# <LINK_LIBRARIES> are the runtime of every program of the machine.
set(CMAKE_C_LINK_EXECUTABLE
    "sh \"${MICROOS_SDK_ROOT}/bin/link_program.sh\" \
--cc \"${CMAKE_C_COMPILER}\" \
--script \"${MICROOS_SDK_ROOT}/lib/program.ld\" \
--entry _start \
--out <TARGET> \
--base ${MICROOS_PROGRAM_BASE} \
--arch x86-64 \
--mkprog \"${MICROOS_SDK_ROOT}/bin/mkprog.sh\" \
-- <LINK_FLAGS> ${MICROOS_GC_LINK_FLAGS} <OBJECTS> <LINK_LIBRARIES> \
\"${MICROOS_SDK_ROOT}/lib/crt0.o\" \"${MICROOS_SDK_ROOT}/lib/libmicroos_user.a\"")
