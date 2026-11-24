# Micro OS — the programs of `make ports-smoke`, built with the SDK alone.
#
# This project is deliberately not a part of the tree's CMake: it is what a
# person with `build-sdk/microos-sdk/<version>/` writes for a program of
# somebody else, and `make ports-smoke` runs it with the toolchain file of the
# SDK (`share/microos-toolchain.cmake`) and nothing of the build of the tree.
# The sources under `sbase/` are the release of sbase, unchanged: a program
# that needed a line of this file to be edited would be a patch of upstream,
# and the gate of the lane is that there is none (docs/ports-table.md).
#
# One archive for the helpers of the collection (`libutil`: eprintf, fshut,
# putword, ealloc, strtonum, writeall) and one program per tool. A tool that
# does not call a helper does not link it: the linker of the image takes the
# members the program names.
cmake_minimum_required(VERSION 3.16)
project(microos_ports C)

set(SBASE "${CMAKE_CURRENT_SOURCE_DIR}/sbase")

# The helpers of the collection. They are the upstream files of `libutil/`,
# copied byte for byte; `upstream.txt` carries their sha256.
add_library(sbase_libutil STATIC
    sbase/libutil/eprintf.c
    sbase/libutil/fshut.c
    sbase/libutil/putword.c
    sbase/libutil/ealloc.c
    sbase/libutil/strtonum.c
    sbase/libutil/writeall.c
    # `sha256sum` carries a library of its own, and the library is the
    # release's: the hash and the driver of the sums of the collection.
    sbase/libutil/sha256.c
    sbase/libutil/crypt.c
)
target_include_directories(sbase_libutil PRIVATE "${SBASE}")

# The tools of the gate, in the order of the table of `docs/ports-table.md`:
# the program of the image is named after the file of the release. `whoami`
# asks for `getpwuid` and `sha256sum` carries the hash of the collection, so
# the two are the calls and the library of a program as well as its console.
set(SBASE_TOOLS
    true
    false
    echo
    basename
    dirname
    pwd
    printenv
    sleep
    cksum
    tee
    cmp
    whoami
    sha256sum
)

foreach(tool IN LISTS SBASE_TOOLS)
    add_executable(${tool} "${SBASE}/${tool}.c")
    target_include_directories(${tool} PRIVATE "${SBASE}")
    target_link_libraries(${tool} PRIVATE sbase_libutil)
endforeach()

# The one program of this directory that is ours: the gate of `popen`,
# `pclose` and `system` on the machine. It is not a port of anything - the
# three calls are the library of the tree - and the eleven above do not know
# it exists.
add_executable(command command.c)
