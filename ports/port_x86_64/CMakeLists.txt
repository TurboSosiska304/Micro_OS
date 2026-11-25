# Micro OS — the x86-64 port as a project of its own.
#
# There is no libc, no POSIX and no operating system under the kernel. Two
# images are built:
#
#   microos64.bin  the kernel itself, linked in the higher half (the top
#                  2 GiB) and copied by the loader to 16 MiB, a flat binary;
#   loader32.elf   the bootloader of the port, a multiboot image that QEMU
#                  loads with -kernel, which copies the kernel and turns the
#                  long mode on.
#
#   cmake -B build-x86 -S ports/port_x86_64 -G Ninja
#   cmake --build build-x86
#   qemu-system-x86_64 -kernel build-x86/loader32.elf \
#                      -initrd build-x86/microos64.bin -m 256 \
#                      -display gtk -serial none

cmake_minimum_required(VERSION 3.16)

project(micro_os_x86_64 C ASM)

# The build id and the paths are the same on two builds of one tree when
# SOURCE_DATE_EPOCH is set (make exports it); this is the same module the
# host build uses.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/reproducible.cmake")

set(KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kernel")
set(KSHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/kshell")
set(LIBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/libc")
set(MFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/mfs")
set(FAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/fs_fat32")
set(EXT2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/fs_ext2")
set(PROCFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/procfs")
set(DEVFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/devfs")
set(CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/config")
set(TELNET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/telnet")
set(SETTINGS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/settings")

# The second filesystem of the machine (ADR 0009). On: the boot of the root
# reads the type of the volume out of the medium (ADR 0027), so the machine
# that is told to run on a FAT32 volume has the reader of it in the image it
# was started with, and the second partition of the ext2 disk is a /data a
# person may fill from another computer.
option(MICRO_OS_FS_FAT32 "FAT12/16/32, read and written" ON)

# The ext2 filesystem of the machine (ADR 0019). On for the same reason: the
# release carries the same tree twice, once in MicroFS and once in ext2, and
# one kernel boots both (ADR 0027 §3).
option(MICRO_OS_FS_EXT2 "ext2, read and written" ON)

# The optional features of the image (ADR 0010): the PSRAM pool of the
# allocator is off, the MOX1 runtime is on (the port can enter a program), and
# the two wireless services of the board are off. Their flags are written
# here so that the defaults of the port are in one place.
option(KERNEL_PSRAM "the PSRAM pool and its statistics in kmm" OFF)
option(KERNEL_EXEC "the MOX1 parser, execve and the exec command" ON)
option(KERNEL_SMP "the second core: a run queue per core (ADR 0012)" OFF)
option(MICRO_OS_TELNET "the shell on TCP port 23" OFF)
option(MICRO_OS_WIFI "the WiFi station" OFF)

# The network of the machine (K6): the port has its own IPv4 stack and a
# virtio-net driver (src/net.c, src/virtio_net.c), which are always part of
# the image and cost nothing when QEMU was given no card. This switch adds
# the commands of components/net - `http`, `httpd` and `ntp` - on top of that
# stack. It is off by default: the smoke test of the machine keeps the same
# command list, and `make x86-net-smoke` builds a second image with it on.
option(MICRO_OS_X86_NET "the http, httpd and ntp commands of components/net" OFF)

# The policy of the signed image (ADR 0020): on, the loader refuses an image
# that did not pass and no byte of it runs; off, the loader still verifies,
# prints the warning and continues. The second setting is for a development
# machine whose image is not signed yet; the smoke test builds both and boots
# an unsigned kernel on the second one.
option(MICRO_OS_REQUIRE_SIGNATURE "the boot refuses an image without a valid signature" ON)
if(MICRO_OS_REQUIRE_SIGNATURE)
    set(MICRO_OS_REQUIRE_SIGNATURE_VALUE 1)
else()
    set(MICRO_OS_REQUIRE_SIGNATURE_VALUE 0)
endif()

set(NET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/net")
set(PKG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/pkg")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The kernel of the higher half: no red zone (an interrupt may use the stack
# under us), no PIC, no SSE in the C of the kernel (the state of the units is
# saved by the switch of the port; a program that asks for them gets them
# from the loader, see USERLAND_CFLAGS below), and the kernel memory model,
# which names the top 2 GiB where the image is
# linked and leaves the low identity map to the pointers the boot hands over
# (ADR 0007). It is a flag of the C code alone: the assembly is written in
# absolute 64-bit numbers and the programs of /bin stay in the low window.
#
# -mgeneral-regs-only is the rule of the kernel and of a program that does not
# ask for a double; it is set on the target of the kernel and in PROGRAM_CFLAGS
# below and not in this directory-wide list, because a ported library with a
# point in its numbers (Lua) is built without it on purpose.
add_compile_options(
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -mno-red-zone
    # The machine is the first x86-64 and not the computer of the person who
    # builds it. A distribution may configure its compiler for a newer model
    # (the runner of GitHub does) and then a shift becomes SHLX of BMI2,
    # which the processor of QEMU does not know: an invalid opcode in the
    # middle of the boot. The explicit model is the oldest one.
    -march=x86-64
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    # The kernel has no libc, and some distributions turn the checks of the
    # C library into calls to __snprintf_chk and friends: there is nobody to
    # answer them under the kernel.
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
)

file(GLOB KERNEL_SOURCES "${KERNEL_DIR}/src/*.c")

# The shell is the kernel's own interpreter: the port supplies the console
# under it and nothing else, so the commands are the same as on the board.
set(KSHELL_SOURCES
    "${KSHELL_DIR}/src/kshell.c"
    "${KSHELL_DIR}/src/kshell_files.c"
    "${KSHELL_DIR}/src/kshell_memtest.c"
    "${KSHELL_DIR}/src/kshell_mfetch.c"
    "${KSHELL_DIR}/src/kshell_time.c"
    "${KSHELL_DIR}/src/kshell_pipe.c"
)

# The parts of the C library the shell calls. They reach the kernel through
# the ABI of the system calls and know nothing about the port.
#
# src/stdio.c is left out on purpose: the path of its %f conversions needs
# SSE registers, and the port is built with -mgeneral-regs-only because
# nothing here saves the XMM state. The formatter of src/libc_shim.c serves
# the shell instead; it speaks the conversions the commands use, without the
# precision and the '*' widths that src/stdio.c adds.
set(LIBC_SOURCES
    "${LIBC_DIR}/src/stdlib.c"
    "${LIBC_DIR}/src/unistd.c"
)

# MicroFS, so that the machine has a tree of directories and a program in
# /bin. The filesystem is the same code the board runs, mounted on the RAM
# volume the port registers (src/blockdev_ram.c). src/mfs_ram_device.c is not
# here: that one is the volume of the host tests, and it takes its memory from
# the libc of the process.
set(MFS_SOURCES
    "${MFS_DIR}/src/mfs.c"
    "${MFS_DIR}/src/mfs_check.c"
    "${MFS_DIR}/src/mfs_vfs.c"
    "${MFS_DIR}/src/mfs_vfs_mount.c"
)

set(FAT_SOURCES)
if(MICRO_OS_FS_FAT32)
    set(FAT_SOURCES
        "${FAT_DIR}/src/fat_core.c"
        "${FAT_DIR}/src/fat_dir.c"
        "${FAT_DIR}/src/fat_check.c"
        "${FAT_DIR}/src/fat_vfs.c"
    )
endif()

set(EXT2_SOURCES)
if(MICRO_OS_FS_EXT2)
    set(EXT2_SOURCES
        "${EXT2_DIR}/src/ext2_core.c"
        "${EXT2_DIR}/src/ext2_dir.c"
        "${EXT2_DIR}/src/ext2_write.c"
        "${EXT2_DIR}/src/ext2_check.c"
        "${EXT2_DIR}/src/ext2_vfs.c"
    )
endif()

# The two virtual filesystems of the tree: the information about the system
# (/proc) and the devices (/dev). The board and the simulator mount them at
# the boot; the x86-64 machine does the same, so the tree of the three is the
# same tree and the shell of this port has the files the wiki shows.
set(VIRTUAL_SOURCES
    "${PROCFS_DIR}/src/procfs.c"
    "${DEVFS_DIR}/src/devfs.c"
)

# The configuration of /etc and its systems (ADR 0021). This machine has no
# store of its own: the port answers "there is no key", the files of the
# volume are the truth, and `config set` edits them as it does on the board.
set(SETTINGS_SOURCES
    "${SETTINGS_DIR}/src/settings.c"
    "${CONFIG_DIR}/src/config.c"
    "${CONFIG_DIR}/src/config_cmd.c"
    "${CONFIG_DIR}/src/config_users.c"
    "${CONFIG_DIR}/src/config_login.c"
    "${CONFIG_DIR}/src/config_session.c"
)

# The font of the console is the one of the PC (port_x86_64_font.h, Spleen
# 8x16, third_party/spleen): the 5x7 page of components/font is the panel of
# the board, whose letters are the wrong size for a screen of 1280x800 and
# whose lowercase is the upper case. The table of the port is src/font_spleen.c
# and the license travels with the bundle (tools/bundle.sh).

# The services over the sockets: the HTTP client, the page of the state and
# the clock of NTP. They are portable and speak kernel/net.h, so the same
# sources that run on the board run here over the stack of the port. The
# clients are off by default (MICRO_OS_X86_NET); the sources are named here
# and not by the CMakeLists of the component, which is written for the tree of
# ESP-IDF. src/dns.c is the exception: the stack of the port asks it for every
# name, in every image, and it is the cache the contract of K6 asks for.
set(NET_SOURCES "${NET_DIR}/src/dns.c" "${NET_DIR}/src/dhcp.c")

if(MICRO_OS_X86_NET)
    list(APPEND NET_SOURCES
        "${NET_DIR}/src/net.c"
        "${NET_DIR}/src/http.c"
        "${NET_DIR}/src/ntp.c"
        "${NET_DIR}/src/net_http_server.c"
        "${NET_DIR}/src/httpd.c"
        "${NET_DIR}/src/tls.c"
        "${NET_DIR}/src/tls_server.c"
        # The update over the network: the download and its check are
        # portable, and the slots of this machine are files (/var/ota).
        "${NET_DIR}/src/ota.c"
        "${NET_DIR}/src/ota_file.c"
    )
endif()

# The TLS stack of the machine (ADR 0024 §3): mbedTLS is vendored and its
# configuration is third_party/mbedtls/config/microos_config.h. The library
# is an object library, so an image that links it carries its bytes; the
# layer of the platform (the timer and RDRAND, src/tls_platform.c) and the
# client (components/net/src/tls.c) are part of the network image.
if(MICRO_OS_X86_NET)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/mbedtls" mbedtls)
    # The kernel is linked in the higher half: the library's static data has
    # to be addressed the same way as the kernel's own.
    target_compile_options(microos_mbedtls PRIVATE -mcmodel=kernel)
    # -O2 and not the -O3 of the image: the library at -O3 with this
    # compiler and these freestanding flags fails the Finished check of
    # TLS 1.3 (the smoke test found it, the debug build and -O2 pass). -O2
    # is the upstream default; the library is not the place to chase an
    # optimizer, and a handshake that fails on one image is not a choice.
    target_compile_options(microos_mbedtls PRIVATE -O2)
endif()

# The installer of programs (ADR 0024). It is part of the network image: a
# package arrives over HTTP, and `pkg` is the half of the promise of 2.0 that
# a machine changes itself. The signature of a package is the signature of
# the image, so the two primitives of the loader (SHA-256 and Ed25519) are
# compiled into the kernel as well, and the public key of the build is the
# key of the repository when /etc/pkg/keys has none (ADR 0024 §4).
set(PKG_SOURCES)
if(MICRO_OS_X86_NET)
    list(APPEND PKG_SOURCES
        "${PKG_DIR}/src/pkg.c"
        "${PKG_DIR}/src/pkg_io.c"
        "${PKG_DIR}/src/pkg_db.c"
        "${PKG_DIR}/src/pkg_index.c"
        "${PKG_DIR}/src/pkg_verify.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/loader/ed25519.c"
    )
endif()

# The shell over the air (ADR 0033, the second half of the accounts): the
# port of this machine on the network of QEMU, with the accounts of /etc. It
# is off by default and on in the machine of `make x86-net-smoke` and of
# `make x86-all`, where there is a card to listen on.
set(TELNET_SOURCES)
if(MICRO_OS_TELNET)
    list(APPEND TELNET_SOURCES
        "${TELNET_DIR}/src/telnet_proto.c"
        "${TELNET_DIR}/src/telnet_server.c"
    )
endif()

# The hash of the machine: SHA-256 of the loader, which verifies a signed image
# and the hash of a password of /etc/shadow (components/config). It is in every
# image of this port and not only in the one that installs packages.
set(HASH_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/loader/sha256.c")

# The memory test calls strtoul without including the header of the library;
# on the board that resolves to the libc of ESP-IDF. Here the name has to
# point at the conversion of the system, so the header is put in front of
# that one file and its macros do the renaming.
set_source_files_properties("${KSHELL_DIR}/src/kshell_memtest.c"
    PROPERTIES COMPILE_OPTIONS "-include;libc/stdlib.h")

# --- the program of the port: /bin/hello -------------------------------------
#
# The image has to be inside the kernel before there is a filesystem to read it
# from, so it is built here and linked in as a blob. Three steps, and the
# second one is the interesting one: tools/mkprog.sh copies the sections of an
# object as they are, and a reference to a string is a relocation until a
# linker has applied it. The object is therefore linked first, for the window
# of the programs (programs/program.ld), and the image is made out of the
# linked file.
#
# program.ld puts the text at the base of the window plus the header of the
# format (0x02000020). The address is a symbol of the script and not a number
# the script can be trusted to reach: the ld of binutils 2.38 (the container of
# the release) moves the text 32 bytes up, and 2.47 keeps it. tools/
# link_program.sh therefore links, reads the address back and links again with
# the value the linker really used; it is the only caller of the compiler's
# linker for a program of this port. The base is named here again and handed
# on, and mkprog.sh refuses an image linked for another window: a program
# linked for the wrong address would still run, and every absolute address it
# computes for its data would name something else.
#
# The machine of the image is named as well: --arch x86-64 is the number the
# kernel compares with k_port_exec_arch() before it enters a byte, so that the
# image of this window is never mistaken for the code of another machine
# (ADR 0018). The default of mkprog.sh is the same number; the flag is written
# out so that a port that builds for two machines says which one this is.
set(PROGRAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/programs")
# The base of the window is the one of the port (PORT_X86_64_USER_BASE in
# include/port_x86_64.h): a program is linked where it will be mapped, and the
# 96 MiB of ADR 0057 moved the window from 32 MiB to 256 MiB. The two numbers
# are one promise and a test of the image (`mkprog.sh` refuses an image of
# another base) is what keeps them together.
set(PROGRAM_BASE 0x10000000)
set(MKPROG "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mkprog.sh")
set(LINK_PROGRAM "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/link_program.sh")

# The library of a program (components/libc/user): `_start`, the door of the
# syscall, the formatter and the strings. It is not the library of the kernel
# (${LIBC_DIR}/src), which is linked into the kernel and calls ksys_*: a
# program has no such address, only `int $0x80`. Every program that uses it
# carries its own copy, which is what "statically linked" means here.
set(LIBC_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/libc/user")

# The flags of a guest object: no libc, no PIC and no SSE, the same shape as
# the kernel's (see add_compile_options above). The include path carries the
# headers of the kernel (the numbers of the calls) and the library.
#
# The string literals are not merged into one section: binutils 2.47 trips an
# assertion of its merge of sections on the .rodata of a program with many
# strings and a pointer of a handler in its data, and one shared copy of a few
# bytes buys nothing here.
#
# A program of the tree that does not compute in floating point keeps
# -mgeneral-regs-only. A ported program that does (Lua) is built with
# MICROOS_FLOAT_CFLAGS instead: the port sets CR4.OSFXSR and CR0.NE for the
# XMM registers and the x87 unit, and the scheduler saves the state of a task
# at every switch (docs/debts.md, K9), so two such programs may compute at
# once. The kernel itself never touches them.
set(USERLAND_CFLAGS
    -O2 -ffreestanding -fno-stack-protector
    -fno-pic -fno-pie -mno-red-zone -march=x86-64
    -fno-asynchronous-unwind-tables -fno-merge-constants -Wall -Wextra -Werror
)
set(PROGRAM_CFLAGS -c ${USERLAND_CFLAGS} -mgeneral-regs-only
    -I "${KERNEL_DIR}/include" -I "${PROGRAM_DIR}" -I "${LIBC_USER_DIR}/include"
)

# A program with `double` in its numbers (SQLite and its REAL type here, Lua
# on its own branch) is built without -mgeneral-regs-only: the loader turns
# the SSE and x87 units on for ring 3 (CR4.OSFXSR, CR0.NE) and the scheduler
# saves and loads the state of the units at every switch (docs/debts.md, K9),
# so a program that computes at once with another keeps its own registers.
# The kernel and every program without a double still never touch them.
set(MICROOS_FLOAT_CFLAGS ${USERLAND_CFLAGS})
list(REMOVE_ITEM MICROOS_FLOAT_CFLAGS -mgeneral-regs-only)
set(MICROOS_FLOAT_PROGRAM_CFLAGS -c ${MICROOS_FLOAT_CFLAGS}
    -I "${KERNEL_DIR}/include" -I "${PROGRAM_DIR}" -I "${LIBC_USER_DIR}/include"
)

# The recipe of a program and of a ported library: the runtime, the window,
# the image of the format and a static archive of the guest. It lives in
# cmake/userland.cmake so that the next port (Lua, sbase, SQLite) is a copy
# of one call and not a copy of this file.
set(MICROOS_USERLAND_CFLAGS "${USERLAND_CFLAGS}")
set(MICROOS_PROGRAM_CFLAGS "${PROGRAM_CFLAGS}")
set(MICROOS_KERNEL_INCLUDE "${KERNEL_DIR}/include")
set(MICROOS_LIBC_USER_DIR "${LIBC_USER_DIR}")
set(MICROOS_PROGRAM_DIR "${PROGRAM_DIR}")
set(MICROOS_PROGRAM_BASE "${PROGRAM_BASE}")
set(MICROOS_MKPROG "${MKPROG}")
set(MICROOS_LINK_PROGRAM "${LINK_PROGRAM}")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/userland.cmake")

# Every program of the port is built the same way: compile the C file for the
# window, link it with the script of the format, and turn the sections into a
# MOX1 image. The image is a file of the build, and `make x86-disk` writes it
# into the MicroFS volume of the disk: the kernel has no blob of a program in
# it any more. The name of the function is the name of the file in /bin and
# the symbol of the entry is given here.
#
# The link and the image are two steps of one script: tools/link_program.sh
# owns the flags of the format, the address of the text (with the retry a
# linker of another version needs) and the call of mkprog.sh. A caller names
# the script, the entry and the base, and appends the objects after `--`.
function(add_program_image name entry)
    set(image "${CMAKE_BINARY_DIR}/${name}.mox")

    add_custom_command(
        OUTPUT "${image}"
        COMMAND ${CMAKE_C_COMPILER} ${PROGRAM_CFLAGS}
                -o ${name}.o "${PROGRAM_DIR}/${name}.c"
        COMMAND sh "${LINK_PROGRAM}" --cc "${CMAKE_C_COMPILER}"
                --script "${PROGRAM_DIR}/program.ld" --entry ${entry}
                --out ${name}.mox --base ${PROGRAM_BASE} --arch x86-64
                --mkprog "${MKPROG}" -- ${name}.o
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS "${PROGRAM_DIR}/${name}.c" "${PROGRAM_DIR}/prog.h"
                "${PROGRAM_DIR}/program.ld" "${KERNEL_DIR}/include/kernel/ksys.h"
                "${MKPROG}" "${LINK_PROGRAM}"
        COMMENT "building the image of /bin/${name}"
        VERBATIM
    )

    set(MICROOS_PROGRAM_IMAGES ${MICROOS_PROGRAM_IMAGES} "${image}" PARENT_SCOPE)
endfunction()

add_program_image(hello hello_main)
add_program_image(door bench_main)
add_program_image(crash crash_main)
add_program_image(fork fork_main)
add_program_image(badptr badptr_main)
add_program_image(signals signals_main)
add_program_image(nx nx_main)

# The attacks of the policy of a page (ADR 0052, `make mmu-smoke`): each one
# breaks one rule of the map and is expected to end its own task with EFAULT,
# except the two that ask for more than the policy allows and are answered by
# the call - a writable and executable mapping, and a heap past the limit of
# the task. The suite also reads the stack that grows by 128 KiB.
add_program_image(mmu_text mmu_text_main)
add_program_image(mmu_execstack mmu_execstack_main)
add_program_image(mmu_kernel mmu_kernel_main)
add_program_image(mmu_heap mmu_heap_main)
add_program_image(mmu_wx mmu_wx_main)
add_program_image(mmu_stray mmu_stray_main)
add_program_image(mmu_stack mmu_stack_main)
add_program_image(mmu_io mmu_io_main)

# The fork of a program of 64 MiB (ADR 0057): the pages are shared and not
# copied, the child writes four of them, the parent reads the old ones, and
# the port's line carries the count of the shared pages and the time of the
# call. `make fork-smoke` types the line and reads it.
add_program_image(forksmoke forksmoke_main)

# A program that uses the library of a program: the runtime sources are
# compiled for the window together with the file of the program, linked with
# the same script, and the entry is `_start` of the library - main() is a
# function of the program, not the place the port enters (ADR 0016). The
# steps are in cmake/userland.cmake; this wrapper keeps the calls of the tree
# one word long.
function(add_libc_program name)
    microos_add_userland_program(${name} ${ARGN})
    set(MICROOS_PROGRAM_IMAGES ${MICROOS_PROGRAM_IMAGES} PARENT_SCOPE)
endfunction()

# --- zlib: the first program of somebody else -------------------------------
#
# The library is built by the compiler of the guest with the flags of the
# guest and the port layer of `third_party/zlib/port` (the configuration of
# zconf and the two allocation hooks). It is a static archive: `zcat` links
# the objects it uses, and no symbol of the host libc enters the image.
set(ZLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/zlib")
set(ZLIB_SOURCES
    "${ZLIB_DIR}/adler32.c"
    "${ZLIB_DIR}/compress.c"
    "${ZLIB_DIR}/crc32.c"
    "${ZLIB_DIR}/deflate.c"
    "${ZLIB_DIR}/gzclose.c"
    "${ZLIB_DIR}/gzlib.c"
    "${ZLIB_DIR}/gzread.c"
    "${ZLIB_DIR}/gzwrite.c"
    "${ZLIB_DIR}/infback.c"
    "${ZLIB_DIR}/inffast.c"
    "${ZLIB_DIR}/inflate.c"
    "${ZLIB_DIR}/inftrees.c"
    "${ZLIB_DIR}/trees.c"
    "${ZLIB_DIR}/uncompr.c"
    "${ZLIB_DIR}/zutil.c"
    "${ZLIB_DIR}/port/zlib_os.c"
)

microos_userland_library(microos_user_zlib
    SOURCES ${ZLIB_SOURCES}
    INCLUDES "${ZLIB_DIR}" "${ZLIB_DIR}/port"
    DEFINES MY_ZCALLOC HAVE_UNISTD_H HAVE_STDARG_H
    # The library is upstream C and is compiled with the warnings of this
    # tree: the one it has is the one every zlib build answers, and the
    # exception stays here rather than in the vendored source.
    COMPILE_OPTIONS "-include" "${ZLIB_DIR}/port/zconf.h" -Wno-sign-compare -Os
)

# The two wrappers of the port are the only file of the library that calls
# the real read and write: the force-included zconf.h renames them for every
# other source.
set_source_files_properties("${ZLIB_DIR}/port/zlib_os.c"
    PROPERTIES COMPILE_DEFINITIONS ZLIB_OS_NO_RENAME=1)
# A force-included header is not in the compiler's list of includes: without
# this a change to the configuration of the port would not rebuild a source.
set_source_files_properties(${ZLIB_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${ZLIB_DIR}/port/zconf.h")

add_libc_program(echo)
add_libc_program(mkdir)
add_libc_program(mv)
add_libc_program(pwd)
# The two removals call the POSIX wrappers of unistd_ext.c, which the small
# runtime of a program does not carry: the object is linked into these two
# images alone.
add_libc_program(rm SOURCES "${LIBC_USER_DIR}/src/unistd_ext.c"
                            "${LIBC_USER_DIR}/src/stat.c")
add_libc_program(rmdir SOURCES "${LIBC_USER_DIR}/src/unistd_ext.c"
                               "${LIBC_USER_DIR}/src/stat.c")
add_libc_program(spawn)
# The gates of the first lane of 1.6: `poll` waits for a descriptor in the
# scheduler, `mmap` reads a file through a mapping and writes it back, and
# `big` measures the memory a program of 4 MiB costs. The three call the
# POSIX wrappers of unistd_ext.c (pipe, fork, poll, mmap, brk), which the
# small runtime does not carry.
add_libc_program(poll SOURCES "${LIBC_USER_DIR}/src/unistd_ext.c"
                           "${LIBC_USER_DIR}/src/stat.c"
                           "${LIBC_USER_DIR}/src/usleep.c")
add_libc_program(mmap SOURCES "${LIBC_USER_DIR}/src/unistd_ext.c"
                              "${LIBC_USER_DIR}/src/stat.c")
add_libc_program(big)

# The keyboard as events (ADR 0040): the smoke test of the USB types a key and
# reads the code and the edge of it out of /dev/input0, which is the promise
# the two keyboards of the machine share.
add_libc_program(keys SOURCES "${LIBC_USER_DIR}/src/usleep.c")

# The lane of 1.8: the directory of the task is the kernel's (call 48), so a
# program moves it and its child starts there; the heap of a program grows
# with `brk`, so a window of 800 by 600 pixels does not need a constant of the
# build; and the memory limit of the task is what the new heap is charged to.
add_libc_program(chdir SOURCES "${LIBC_USER_DIR}/src/unistd_ext.c"
                              "${LIBC_USER_DIR}/src/stat.c")
add_libc_program(heap SOURCES "${LIBC_USER_DIR}/src/malloc.c")
add_libc_program(limit SOURCES "${LIBC_USER_DIR}/src/malloc.c"
                               "${LIBC_USER_DIR}/src/resource.c")

# --- the interface: /bin/ui (ADR 0028) and the programs of ADR 0030 ---------
#
# The first program of the skeleton: two devices of the devfs, a panel, a list
# of windows and a window of the log. The font comes from the console of the
# port and not from a second table, so the letters of the interface are the
# letters of the shell and the picture of a screenshot is read with one table.
# The program allocates nothing: its buffers are part of the bss of the image.
#
# The window model and the keyboard are `components/ui`, a library of the
# userland with a test of its own on the host: the skeleton keeps the model of
# its two windows, and the editor and the file manager of ADR 0030 are the
# programs that use it whole. `programs/uiprog.c` is the port's half - the
# writes of /dev/fb0 cut into transfers - so the library stays portable.
set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui")
set(UI_INCLUDES "${UI_DIR}/include" "${UI_DIR}/client/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/include")


add_libc_program(ui
    MAIN "${PROGRAM_DIR}/ui.c"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/font_spleen.c"
            "${LIBC_USER_DIR}/src/usleep.c"
            "${UI_DIR}/src/window.c"
    INCLUDES ${UI_INCLUDES}
)

add_libc_program(edit
    MAIN "${PROGRAM_DIR}/edit.c"
    SOURCES "${PROGRAM_DIR}/uiprog.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/font_spleen.c"
            "${LIBC_USER_DIR}/src/usleep.c"
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${UI_DIR}/src/window.c"
            "${UI_DIR}/src/draw.c"
            "${UI_DIR}/src/palette.c"
            "${UI_DIR}/src/clock.c"
            "${UI_DIR}/src/keyboard.c"
            "${UI_DIR}/client/src/wm_protocol.c"
            "${UI_DIR}/client/src/wm_client.c"
    INCLUDES ${UI_INCLUDES}
)

# --- /bin/wm, the server of the desktop (1.7, docs/desktop.md) --------------
#
# The program that owns the screen: it paints the wallpaper, the panel and the
# pointer, starts its clients with fork and execve, and copies the buffer of
# every window into the picture. The protocol and the client of it are
# `components/ui/client` - portable and tested on the host - and this program
# is the server of that contract plus the picture of the machine.
add_libc_program(wm
    MAIN "${PROGRAM_DIR}/wm.c"
    SOURCES "${PROGRAM_DIR}/uiprog.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/font_spleen.c"
            "${LIBC_USER_DIR}/src/usleep.c"
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${LIBC_USER_DIR}/src/time.c"
            "${UI_DIR}/src/window.c"
            "${UI_DIR}/src/draw.c"
            "${UI_DIR}/src/palette.c"
            "${UI_DIR}/src/clock.c"
            "${UI_DIR}/src/keyboard.c"
            "${UI_DIR}/client/src/wm_protocol.c"
            "${UI_DIR}/client/src/wm_client.c"
    INCLUDES ${UI_INCLUDES}
)

# --- /bin/term, the terminal window of the desktop (2.0, ADR 0054) ----------
#
# The window that runs the programs of /bin: a client of the server like the
# editor, a grid of cells of `components/ui/src/term.c`, and a child process on
# a pair of pipes. The grid is the portable half of the terminal and the host
# test holds it (`tests/cases/test_ui_term.c`); this program is the picture of
# the cells and the pipes.
add_libc_program(term
    MAIN "${PROGRAM_DIR}/term.c"
    SOURCES "${PROGRAM_DIR}/uiprog.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/font_spleen.c"
            "${LIBC_USER_DIR}/src/usleep.c"
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${UI_DIR}/src/window.c"
            "${UI_DIR}/src/draw.c"
            "${UI_DIR}/src/palette.c"
            "${UI_DIR}/src/clock.c"
            "${UI_DIR}/src/keyboard.c"
            "${UI_DIR}/src/term.c"
            "${UI_DIR}/client/src/wm_protocol.c"
            "${UI_DIR}/client/src/wm_client.c"
    INCLUDES ${UI_INCLUDES}
)

add_libc_program(fm
    MAIN "${PROGRAM_DIR}/fm.c"
    SOURCES "${PROGRAM_DIR}/uiprog.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/font_spleen.c"
            "${LIBC_USER_DIR}/src/usleep.c"
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${LIBC_USER_DIR}/src/dirent.c"
            "${LIBC_USER_DIR}/src/malloc.c"
            "${UI_DIR}/src/window.c"
            "${UI_DIR}/src/draw.c"
            "${UI_DIR}/src/palette.c"
            "${UI_DIR}/src/clock.c"
            "${UI_DIR}/src/keyboard.c"
            "${UI_DIR}/client/src/wm_protocol.c"
            "${UI_DIR}/client/src/wm_client.c"
    INCLUDES ${UI_INCLUDES}
)

# The first program of somebody else: zcat opens a .gz of the VFS through
# zlib's gz* layer, inflates it and writes the bytes to the console or a
# file. The heap of the program is 512 KiB because the state of an inflate
# stream is a 32 KiB window and the buffers of gz are two blocks of 8 and 16
# KiB; the arena is the bss of the image, inside the window of the port.
add_libc_program(zcat
    LIBRARIES microos_user_zlib
    INCLUDES "${ZLIB_DIR}"
    DEFINES ULIBC_HEAP_SIZE=524288u
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"
            "${LIBC_USER_DIR}/src/file.c"
    # The image of a program is read whole by the kernel in a block of
    # KSYS_MAX_IO (64 KiB): the text and the data of zcat and its library have
    # to fit that, and -Os is the size of zlib that does.
    COMPILE_OPTIONS -Os
)

# --- pcre2grep, the second program of somebody else -------------------------
#
# PCRE2 (third_party/pcre2) is third-party C: the compiler of the guest builds
# it for the window with the flags of the guest, and the linker takes the
# objects the program names. The warnings of this tree are off it, because its
# code is not ours to change and a warning in it is an upstream matter; the
# compatibility headers of the vendor (`third_party/pcre2/compat`) answer the
# includes of a C library that a program here does not have, and the strings,
# the allocation and the classes of a character are the ones of
# `components/libc/user`.
set(PCRE2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pcre2")
set(PCRE2_SOURCES
    "${PCRE2_DIR}/src/pcre2_compile.c"
    "${PCRE2_DIR}/src/pcre2_config.c"
    "${PCRE2_DIR}/src/pcre2_context.c"
    "${PCRE2_DIR}/src/pcre2_convert.c"
    "${PCRE2_DIR}/src/pcre2_dfa_match.c"
    "${PCRE2_DIR}/src/pcre2_error.c"
    "${PCRE2_DIR}/src/pcre2_extuni.c"
    "${PCRE2_DIR}/src/pcre2_find_bracket.c"
    "${PCRE2_DIR}/src/pcre2_maketables.c"
    "${PCRE2_DIR}/src/pcre2_match.c"
    "${PCRE2_DIR}/src/pcre2_match_data.c"
    "${PCRE2_DIR}/src/pcre2_newline.c"
    "${PCRE2_DIR}/src/pcre2_ord2utf.c"
    "${PCRE2_DIR}/src/pcre2_pattern_info.c"
    "${PCRE2_DIR}/src/pcre2_script_run.c"
    "${PCRE2_DIR}/src/pcre2_serialize.c"
    "${PCRE2_DIR}/src/pcre2_string_utils.c"
    "${PCRE2_DIR}/src/pcre2_substitute.c"
    "${PCRE2_DIR}/src/pcre2_substring.c"
    "${PCRE2_DIR}/src/pcre2_tables.c"
    "${PCRE2_DIR}/src/pcre2_ucd.c"
    "${PCRE2_DIR}/src/pcre2_valid_utf.c"
    "${PCRE2_DIR}/src/pcre2_xclass.c"
    "${PCRE2_DIR}/pcre2_chartables.c"
    "${PCRE2_DIR}/pcre2_small.c"
)

microos_userland_library(microos_user_pcre2
    SOURCES ${PCRE2_SOURCES}
    INCLUDES "${PCRE2_DIR}" "${PCRE2_DIR}/src" "${PCRE2_DIR}/compat"
    DEFINES HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8
    COMPILE_OPTIONS -w -Os -ffunction-sections -fdata-sections
)

# The image of pcre2grep is the largest one of the port: the engine, the
# runtime of the library and the program. It stays under the ceiling of the
# format (K_EXEC_MAX_IMAGE, 8 MiB) with room to spare, and -Os keeps it small
# because every byte of the text is a byte of the disk image.
add_libc_program(pcre2grep GC_SECTIONS
    LIBRARIES microos_user_pcre2
    INCLUDES "${PCRE2_DIR}"
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"
            "${LIBC_USER_DIR}/src/ctype.c"
    COMPILE_OPTIONS -Os
)

# --- Lua: the first port of a program with a point in its numbers -----------
#
# The library is the whole core of Lua 5.4.7 - the lexer, the parser, the code
# generator, the virtual machine and the libraries of the release - built as a
# static archive of the guest. The standalone interpreter of the release
# (`lua.c`) is the program: `microos_add_userland_program` takes it through
# MAIN because a ported program has a `main` of its own and the tree does not
# keep a second copy of it in `programs/`.
#
# FLOAT is the difference from every other program: a lua_Number is a double,
# the arithmetic of the machine is SSE and x87, and the flags of this program
# do not carry -mgeneral-regs-only. The runtime of the library is compiled
# with ULIBC_WITH_FLOAT for the same reason: its formatter writes the digits
# of a double, and src/math.c, src/stdlib.c and src/setjmp.c are sources of
# this program alone (a program of the tree never links them, so the image of
# `cat` does not pay for a logarithm).
set(LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/lua")
set(LUA_SOURCES
    "${LUA_DIR}/lapi.c"
    "${LUA_DIR}/lauxlib.c"
    "${LUA_DIR}/lbaselib.c"
    "${LUA_DIR}/lcode.c"
    "${LUA_DIR}/lcorolib.c"
    "${LUA_DIR}/lctype.c"
    "${LUA_DIR}/ldblib.c"
    "${LUA_DIR}/ldebug.c"
    "${LUA_DIR}/ldo.c"
    "${LUA_DIR}/ldump.c"
    "${LUA_DIR}/lfunc.c"
    "${LUA_DIR}/lgc.c"
    "${LUA_DIR}/linit.c"
    "${LUA_DIR}/liolib.c"
    "${LUA_DIR}/llex.c"
    "${LUA_DIR}/lmathlib.c"
    "${LUA_DIR}/lmem.c"
    "${LUA_DIR}/loadlib.c"
    "${LUA_DIR}/lobject.c"
    "${LUA_DIR}/lopcodes.c"
    "${LUA_DIR}/loslib.c"
    "${LUA_DIR}/lparser.c"
    "${LUA_DIR}/lstate.c"
    "${LUA_DIR}/lstring.c"
    "${LUA_DIR}/lstrlib.c"
    "${LUA_DIR}/ltable.c"
    "${LUA_DIR}/ltablib.c"
    "${LUA_DIR}/ltm.c"
    "${LUA_DIR}/lundump.c"
    "${LUA_DIR}/lutf8lib.c"
    "${LUA_DIR}/lvm.c"
    "${LUA_DIR}/lzio.c"
    "${LUA_DIR}/port/lua_os.c"
)

microos_userland_library(microos_user_lua FLOAT
    SOURCES ${LUA_SOURCES}
    INCLUDES "${LUA_DIR}" "${LUA_DIR}/port"
    # The configuration of the port is force-included: it is read before
    # luaconf.h, and the settings of the system (no shell, no tmpnam, no
    # dynamic linker, a point for the radix) are the ones it names. The
    # warnings are the ones upstream C answers in every build.
    COMPILE_OPTIONS "-include" "${LUA_DIR}/port/luaconf_port.h"
        -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers
        -Wno-implicit-fallthrough -Os
)

set_source_files_properties(${LUA_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${LUA_DIR}/port/luaconf_port.h")

add_libc_program(lua
    FLOAT
    LIBRARIES microos_user_lua
    MAIN "${LUA_DIR}/lua.c"
    INCLUDES "${LUA_DIR}" "${LUA_DIR}/port"
    # The arena is the bss of the program and the format caps the bss at a
    # megabyte. Lua's state, its strings and the tables of a script live in
    # it; 768 KiB leaves the state of a real script room and the image under
    # the ceiling.
    DEFINES ULIBC_HEAP_SIZE=786432u
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"
            "${LIBC_USER_DIR}/src/file.c"
            "${LIBC_USER_DIR}/src/stdlib.c"
            "${LIBC_USER_DIR}/src/math.c"
            "${LIBC_USER_DIR}/src/setjmp.c"
            "${LIBC_USER_DIR}/src/time.c"
            "${LIBC_USER_DIR}/src/locale.c"
            "${LIBC_USER_DIR}/src/ctype.c"
            "${LIBC_USER_DIR}/src/assert.c"
            "${LIBC_USER_DIR}/src/format_float.c"
    COMPILE_OPTIONS -DULIBC_WITH_FLOAT -include "${LUA_DIR}/port/luaconf_port.h" -Os
)

# --- zstd: a compression library and the command that speaks it -------------
#
# zstd 1.5.7 is the third library of somebody else, and the first whose
# command is the program of the release itself (`programs/zstdcli.c`) rather
# than a small file of this directory. The port is three targets:
#
#   microos_user_zstd      lib/common, lib/compress and lib/decompress, the
#                          library as the frame format needs it;
#   microos_user_zstd_cli  the four files of `programs/` the command is
#                          (fileio, fileio_asyncio, util, timefn), with the
#                          configuration of `port/zstd_cli_port.h`;
#   zstd                   the release's `zstdcli.c` as the `main`, the
#                          runtime of the library of a program, and the two
#                          archives above.
#
# lib/dictBuilder is not here: the dictionary builder (`--train`) is the one
# mode the command answers "not supported" for, and the library reads a
# dictionary somebody else trained (`-D`). lib/deprecated is not here either:
# the format of 1.0 is not what this machine reads or writes. The library is
# built with `-DZSTD_NO_TRACE -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0` and
# without `ZSTD_MULTITHREAD` (port/zstd_port.h), which is what keeps it inside
# the arena of a program and off the pthread of a host.
set(ZSTD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/zstd")
set(ZSTD_LIB_SOURCES
    "${ZSTD_DIR}/lib/common/debug.c"
    "${ZSTD_DIR}/lib/common/entropy_common.c"
    "${ZSTD_DIR}/lib/common/error_private.c"
    "${ZSTD_DIR}/lib/common/fse_decompress.c"
    "${ZSTD_DIR}/lib/common/pool.c"
    "${ZSTD_DIR}/lib/common/threading.c"
    "${ZSTD_DIR}/lib/common/xxhash.c"
    "${ZSTD_DIR}/lib/common/zstd_common.c"
    "${ZSTD_DIR}/lib/compress/fse_compress.c"
    "${ZSTD_DIR}/lib/compress/hist.c"
    "${ZSTD_DIR}/lib/compress/huf_compress.c"
    "${ZSTD_DIR}/lib/compress/zstd_compress.c"
    "${ZSTD_DIR}/lib/compress/zstd_compress_literals.c"
    "${ZSTD_DIR}/lib/compress/zstd_compress_sequences.c"
    "${ZSTD_DIR}/lib/compress/zstd_compress_superblock.c"
    "${ZSTD_DIR}/lib/compress/zstd_double_fast.c"
    "${ZSTD_DIR}/lib/compress/zstd_fast.c"
    "${ZSTD_DIR}/lib/compress/zstd_lazy.c"
    "${ZSTD_DIR}/lib/compress/zstd_ldm.c"
    "${ZSTD_DIR}/lib/compress/zstd_opt.c"
    "${ZSTD_DIR}/lib/compress/zstd_preSplit.c"
    "${ZSTD_DIR}/lib/decompress/huf_decompress.c"
    "${ZSTD_DIR}/lib/decompress/zstd_ddict.c"
    "${ZSTD_DIR}/lib/decompress/zstd_decompress.c"
    "${ZSTD_DIR}/lib/decompress/zstd_decompress_block.c"
)
set(ZSTD_CLI_SOURCES
    "${ZSTD_DIR}/programs/fileio.c"
    "${ZSTD_DIR}/programs/fileio_asyncio.c"
    "${ZSTD_DIR}/programs/util.c"
    "${ZSTD_DIR}/programs/timefn.c"
    "${ZSTD_DIR}/port/zstd_cli_os.c"
)

# The upstream sources answer warnings of their own (a comparison of an int
# with a size_t, a variable of a build that is not this one): the exceptions
# stay here and not in a vendored file, the way the zlib and the Lua ports
# keep theirs.
microos_userland_library(microos_user_zstd
    SOURCES ${ZSTD_LIB_SOURCES}
    INCLUDES "${ZSTD_DIR}/lib" "${ZSTD_DIR}/port"
    COMPILE_OPTIONS "-include" "${ZSTD_DIR}/port/zstd_port.h"
        -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Os
)
microos_userland_library(microos_user_zstd_cli
    SOURCES ${ZSTD_CLI_SOURCES}
    INCLUDES "${ZSTD_DIR}/lib" "${ZSTD_DIR}/programs" "${ZSTD_DIR}/port"
    COMPILE_OPTIONS "-include" "${ZSTD_DIR}/port/zstd_cli_port.h"
        -Wno-sign-compare -Wno-unused-parameter -Wno-unused-but-set-variable
        -Wno-implicit-fallthrough -Wno-format-truncation -Os
)
set_source_files_properties(${ZSTD_LIB_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${ZSTD_DIR}/port/zstd_port.h")
set_source_files_properties(${ZSTD_CLI_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${ZSTD_DIR}/port/zstd_cli_port.h;${ZSTD_DIR}/port/zstd_port.h")

# The arena of the command is 960 KiB of the megabyte the format allows a bss
# (K_EXEC_BSS_MAX): the pools of the CLI and a context of the library, with
# the room `port/zstd_cli_os.h` measures. The image is `-Os`, about 400 KiB of
# text and 40 KiB of data.
#
# FLOAT is for one mode of the release: `-l` prints the sizes of a frame with
# `%6.2f` (`UTIL_makeHumanReadableSize` answers a double), and a build with
# -mgeneral-regs-only writes those two characters instead of the number. The
# compressor itself is integer arithmetic; the program of the port saves no
# XMM state in the scheduler and `execve` waits for it, which is the rule of
# every program with a point in its numbers (`docs/porting-programs.md`).
add_libc_program(zstd
    FLOAT
    # The CLI first: the linker walks the list once, and the command asks the
    # library of zstd for the symbols of a stream.
    LIBRARIES microos_user_zstd_cli microos_user_zstd
    MAIN "${ZSTD_DIR}/programs/zstdcli.c"
    INCLUDES "${ZSTD_DIR}/lib" "${ZSTD_DIR}/programs" "${ZSTD_DIR}/port"
    DEFINES ULIBC_HEAP_SIZE=983040u
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"
            "${LIBC_USER_DIR}/src/file.c"
            "${LIBC_USER_DIR}/src/stdio_ext.c"
            "${LIBC_USER_DIR}/src/string_ext.c"
            "${LIBC_USER_DIR}/src/strdup.c"
            "${LIBC_USER_DIR}/src/ctype.c"
            "${LIBC_USER_DIR}/src/stdlib.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${LIBC_USER_DIR}/src/dirent.c"
            "${LIBC_USER_DIR}/src/libgen.c"
            "${LIBC_USER_DIR}/src/time.c"
            "${LIBC_USER_DIR}/src/assert.c"
            "${LIBC_USER_DIR}/src/format_float.c"
    # The runtime of this program keeps the raw `open` of the ABI under a name
    # of its own: the port defines the POSIX one for the release (README.md,
    # "The shape of the calls").
    COMPILE_OPTIONS -DULIBC_WITH_FLOAT
        -include "${ZSTD_DIR}/port/zstd_cli_port.h"
        -Dopen=ulibc_open_call -Wno-unused-parameter -Os
)

# --- sbase: the tools of a Unix-like system ---------------------------------
#
# sbase is a collection of small Unix tools by suckless, released under the
# MIT license (third_party/sbase). The tree carries ten of them and the two
# little libraries of the collection: `libutf` (the runes of wc, sort and sed)
# and `libutil` (the helpers of every tool). The upstream files are edited in
# one place - the two lines of `sort.c` that name `sbase_numcmp`, because the
# guest has no floating point registers - and the port layer is
# `third_party/sbase/port`.
#
# The tools are the commands of the machine under their canonical names:
# `/bin/ls` is sbase's `ls`, and the small `ls`, `cat`, `grep`, `wc` and
# `head` of ADR 0016 are not built for this port. `echo`, `mkdir`, `pwd` and
# `rm` stay because the collection has no replacement for them; the smoke
# test reads the sentences of the new commands.
set(SBASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sbase")

set(SBASE_UTF_SOURCES
    "${SBASE_DIR}/libutf/fgetrune.c"
    "${SBASE_DIR}/libutf/fputrune.c"
    "${SBASE_DIR}/libutf/isalnumrune.c"
    "${SBASE_DIR}/libutf/isalpharune.c"
    "${SBASE_DIR}/libutf/isblankrune.c"
    "${SBASE_DIR}/libutf/iscntrlrune.c"
    "${SBASE_DIR}/libutf/isdigitrune.c"
    "${SBASE_DIR}/libutf/isgraphrune.c"
    "${SBASE_DIR}/libutf/isprintrune.c"
    "${SBASE_DIR}/libutf/ispunctrune.c"
    "${SBASE_DIR}/libutf/isspacerune.c"
    "${SBASE_DIR}/libutf/istitlerune.c"
    "${SBASE_DIR}/libutf/isxdigitrune.c"
    "${SBASE_DIR}/libutf/lowerrune.c"
    "${SBASE_DIR}/libutf/rune.c"
    "${SBASE_DIR}/libutf/runetype.c"
    "${SBASE_DIR}/libutf/upperrune.c"
    "${SBASE_DIR}/libutf/utf.c"
    "${SBASE_DIR}/libutf/utftorunestr.c"
)

# The helpers the ten tools reach. The files of the collection that a tool of
# this subset does not call (cp, md5, sha*, estrtod with its `double`) are not
# compiled: a vendored source that is not in a program costs a build and a
# warning and buys nothing.
set(SBASE_LIBUTIL_SOURCES
    "${SBASE_DIR}/libutil/concat.c"
    "${SBASE_DIR}/libutil/ealloc.c"
    "${SBASE_DIR}/libutil/enmasse.c"
    "${SBASE_DIR}/libutil/eprintf.c"
    "${SBASE_DIR}/libutil/eregcomp.c"
    "${SBASE_DIR}/libutil/fnck.c"
    "${SBASE_DIR}/libutil/fshut.c"
    "${SBASE_DIR}/libutil/getlines.c"
    "${SBASE_DIR}/libutil/linecmp.c"
    "${SBASE_DIR}/libutil/memmem.c"
    "${SBASE_DIR}/libutil/mkdirp.c"
    "${SBASE_DIR}/libutil/mode.c"
    "${SBASE_DIR}/libutil/parseoffset.c"
    "${SBASE_DIR}/libutil/putword.c"
    "${SBASE_DIR}/libutil/reallocarray.c"
    "${SBASE_DIR}/libutil/recurse.c"
    "${SBASE_DIR}/libutil/strcasestr.c"
    "${SBASE_DIR}/libutil/strlcat.c"
    "${SBASE_DIR}/libutil/strlcpy.c"
    "${SBASE_DIR}/libutil/strnsubst.c"
    "${SBASE_DIR}/libutil/strsep.c"
    "${SBASE_DIR}/libutil/strtonum.c"
    "${SBASE_DIR}/libutil/unescape.c"
    "${SBASE_DIR}/libutil/writeall.c"
    "${SBASE_DIR}/port/numcmp.c"
    "${SBASE_DIR}/port/humansize.c"
)

microos_userland_library(microos_user_sbase
    SOURCES ${SBASE_UTF_SOURCES} ${SBASE_LIBUTIL_SOURCES}
    INCLUDES "${SBASE_DIR}" "${SBASE_DIR}/port"
    # Upstream code with the warnings of this tree: the comparison of an
    # index with LEN() is the one every sbase build answers, and the
    # exception stays here rather than in a hunk of the vendored file.
    COMPILE_OPTIONS -Wno-sign-compare -Wno-maybe-uninitialized
)

# The library of a program that is not the runtime: the arena, the streams,
# the directory, the information of an object, the clock, the strings of
# POSIX, the regular expressions and the options. It is the same list for
# every tool; the linker takes the objects the tool names.
set(SBASE_LIBC_SOURCES
    "${LIBC_USER_DIR}/src/malloc.c"
    "${LIBC_USER_DIR}/src/file.c"
    "${LIBC_USER_DIR}/src/stdio_ext.c"
    "${LIBC_USER_DIR}/src/ctype.c"
    "${LIBC_USER_DIR}/src/stdlib.c"
    "${LIBC_USER_DIR}/src/string_ext.c"
    "${LIBC_USER_DIR}/src/strdup.c"
    "${LIBC_USER_DIR}/src/dirent.c"
    "${LIBC_USER_DIR}/src/stat.c"
    "${LIBC_USER_DIR}/src/unistd_ext.c"
    "${LIBC_USER_DIR}/src/pwd.c"
    "${LIBC_USER_DIR}/src/time.c"
    "${LIBC_USER_DIR}/src/libgen.c"
    "${LIBC_USER_DIR}/src/termios.c"
    "${LIBC_USER_DIR}/src/regex.c"
    "${LIBC_USER_DIR}/src/getopt.c"
    "${LIBC_USER_DIR}/src/fnmatch.c"
    "${LIBC_USER_DIR}/src/assert.c"
)

# The tool of the collection, built under its own name in /bin: the tools of
# sbase are the `ls`, `cat`, `grep` and the rest of the machine, and the
# programs of the tree they replace are not built for this port. The heap is
# the arena of the program, the bss of the image (capped at a megabyte by the
# format); sort and find hold a file there.
# The test of the streams of the library: `ungetc`, `fread` and a relative
# seek need a kernel under them and no shell command exercises the three
# together, so the machine carries a program that does. It is built with the
# same library as the tools (the streams, the arena, the formatter).
add_libc_program(streams
    MAIN "${PROGRAM_DIR}/streams.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The processes of a program: a child with its own status, a double fork and
# an execve that replaces the child itself. It is the gate of the POSIX pair
# of the library (fork, waitpid, execvp) on the port with an MMU, the way
# programs/fork.c is the gate of copy on write.
add_libc_program(procs
    MAIN "${PROGRAM_DIR}/procs.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The state of the floating point unit at a switch of a task (docs/debts.md,
# K9): three tasks compute in `double` at once and each of them prints the
# numbers of its own registers. FLOAT is the difference from `procs`: the
# loop has to live in the XMM registers for the test to say anything, and the
# library's formatter writes its twelve digits. The program needs the
# formatter of a double, the fork and waitpid of the library and the stat
# that unistd_ext pulls, and nothing of the arena.
add_libc_program(fpu2
    FLOAT
    SOURCES "${LIBC_USER_DIR}/src/format_float.c"
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
    DEFINES ULIBC_HEAP_SIZE=65536u
    COMPILE_OPTIONS -DULIBC_WITH_FLOAT
)

# The test of `ftruncate`: the machine cuts a file of the volume the boot
# mounted, and the smoke test reads the shorter bytes back. It is a program
# and not a command of the shell, because no command of the shell cuts a
# file and the call belongs to the ABI of the programs.
add_libc_program(truncate
    MAIN "${PROGRAM_DIR}/truncate.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The file of the large volume (tools/x86_volume_smoke.py): megabytes
# written and read back, through the double and the triple table of ext2 and
# a chain of thousands of blocks of MicroFS. The buffer is the bss of the
# image and not the heap, so the program costs nothing of the arena of the
# tools and a file of 128 MiB is one chunk of 64 KiB after another.
add_libc_program(mkbig
    MAIN "${PROGRAM_DIR}/mkbig.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The numbers of the machine (tools/x86_bench.py): the call of the door, a
# fork and an exec, a pipe, a file and the console, each of a fixed size that
# the tool passes. The program is not in the image of the release - the tool
# puts it there for the run - because a measurement is a tool of the gate and
# not a command of the machine. It links the same library as the tools, so the
# clock of a program and the fork of the library are the ones the shell and
# every program of /bin already pay.
add_libc_program(bench
    MAIN "${PROGRAM_DIR}/bench.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The ruler of the grain of the I/O (tools/io_bench.py): a file through a
# pipe of the kernel, a stream read a byte at a time and a stream written a
# line at a time. It is a program of a test and not of a person, like
# `mkbig`: the image of the machine does not carry it.
add_libc_program(iobench
    MAIN "${PROGRAM_DIR}/iobench.c"
    SOURCES ${SBASE_LIBC_SOURCES}
    DEFINES ULIBC_HEAP_SIZE=65536u
)

# The two doors of a system call (ADR 0035) are the program `door`, built by
# the rule of the port above and not by the library: a measurement of `int`
# and `syscall` cannot be built on one of the two doors alone.
function(add_sbase_program tool heap)
    add_libc_program(${tool}
        MAIN "${SBASE_DIR}/${tool}.c"
        SOURCES ${SBASE_LIBC_SOURCES}
        LIBRARIES microos_user_sbase
        INCLUDES "${SBASE_DIR}" "${SBASE_DIR}/port"
        DEFINES ULIBC_HEAP_SIZE=${heap}
        COMPILE_OPTIONS -Os -include "${SBASE_DIR}/port/sbase_port.h"
                        -Wno-unused-parameter -Wno-implicit-fallthrough
                        -Wno-sign-compare -Wno-maybe-uninitialized
    )
    # The wrapper inside a function has its own scope: the image of the tool
    # has to be handed up again or the `image` target never builds it.
    set(MICROOS_PROGRAM_IMAGES ${MICROOS_PROGRAM_IMAGES} PARENT_SCOPE)
endfunction()

add_sbase_program(cat 131072u)
add_sbase_program(head 131072u)
add_sbase_program(wc 131072u)
add_sbase_program(tail 131072u)
add_sbase_program(ls 262144u)
add_sbase_program(grep 262144u)
add_sbase_program(sort 524288u)
add_sbase_program(find 524288u)
add_sbase_program(xargs 262144u)
add_sbase_program(sed 524288u)

# --- file and libmagic: the type of a file ----------------------------------
#
# file 5.45 with libmagic (third_party/file, BSD-2-Clause) is the second port
# of a library that reads the bytes of a file and the first one that needs a
# database of its own. The database does not fit: the release ships about ten
# megabytes of magic and the RAM of the machine is what the boot leaves, so
# the port compiles a subset of the Magdir files (tools/magic_subset.py,
# third_party/file/magic/subset.json) into /etc/magic.mgc, and the same text
# travels as /etc/magic for a machine whose compiled file is gone.
#
# The library is a static archive of the guest. It reads the database whole
# with malloc (no QUICK: this machine has no mmap), matches with the portable
# softmagic, and asks the port for the two calls of a process it cannot make
# (port/file_port.c). The types `float` and `double` of the release are left
# out with MICRO_OS_NO_FLOAT: a program of this port has no XMM state.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/libmagic.cmake")

microos_userland_library(microos_user_file
    SOURCES ${MICROOS_FILE_SOURCES}
    INCLUDES "${MICROOS_FILE_DIR}/src" "${MICROOS_FILE_DIR}/port"
    DEFINES HAVE_CONFIG_H
    COMPILE_OPTIONS -Os ${MICROOS_FILE_WARNINGS}
)

# The compiler of the database is a tool of the build and not a program of the
# machine: it is compiled by the compiler of the host with the host libc and
# the same configuration header, so the format of the .mgc it writes is the
# format the library of the guest reads.
add_executable(microos_magic_compile
    ${MICROOS_FILE_SOURCES}
    "${MICROOS_FILE_DIR}/port/magic_compile.c"
)
target_include_directories(microos_magic_compile PRIVATE
    "${MICROOS_FILE_DIR}/src" "${MICROOS_FILE_DIR}/port"
)
# `_GNU_SOURCE` for the tool and not for the guest: the tool is compiled
# against the libc of the host, where `asprintf`, `vasprintf` and
# `strcasestr` are declared only under it, and a compiler of GCC 14 and later
# makes an implicit declaration an error. The library of the guest reads the
# library of the program, which declares them itself.
target_compile_definitions(microos_magic_compile PRIVATE HAVE_CONFIG_H _GNU_SOURCE)
target_compile_options(microos_magic_compile PRIVATE -Os ${MICROOS_FILE_WARNINGS})
target_link_options(microos_magic_compile PRIVATE -no-pie)

# The text of the database and its compiled form. `magic_compile -C magic`
# writes magic.mgc beside the name in its own directory, which is why the
# command runs in the build directory.
set(MAGIC_TEXT "${CMAKE_BINARY_DIR}/magic")
set(MAGIC_MGC "${CMAKE_BINARY_DIR}/magic.mgc")
add_custom_command(
    OUTPUT "${MAGIC_TEXT}"
    COMMAND ${CMAKE_COMMAND} -E env python3
            "${MICROOS_MAGIC_SUBSET}"
            --config "${MICROOS_MAGIC_CONFIG}"
            --out "${MAGIC_TEXT}"
    DEPENDS "${MICROOS_MAGIC_SUBSET}" "${MICROOS_MAGIC_CONFIG}"
            "${MICROOS_FILE_DIR}/magic/microos.magic"
    COMMENT "choosing the subset of the magic database of file"
    VERBATIM
)
add_custom_command(
    OUTPUT "${MAGIC_MGC}"
    COMMAND "$<TARGET_FILE:microos_magic_compile>" -C magic
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS microos_magic_compile "${MAGIC_TEXT}"
    COMMENT "compiling the magic database of /etc"
    VERBATIM
)

# `-z` looks inside the two compressions of the image: gzip through the zlib of
# the program, and zstd through the decompressor of the library the command of
# zstd is built from (`microos_user_zstd`, not the CLI: the archive holds the
# library alone). The linker pulls the objects of the decoder and nothing of
# the compressor, and the port header of the library names the window this
# machine can hold (`port/zstd_port.h`, included by file.c as the sources of
# the library are).
add_libc_program(file
    LIBRARIES microos_user_file microos_user_zlib microos_user_zstd
    INCLUDES "${MICROOS_FILE_DIR}/src" "${MICROOS_FILE_DIR}/port" "${ZLIB_DIR}"
             "${ZSTD_DIR}/lib" "${ZSTD_DIR}/port"
    DEFINES ULIBC_HEAP_SIZE=983040u MICRO_OS_WITH_ZSTD
    SOURCES ${SBASE_LIBC_SOURCES}
            "${LIBC_USER_DIR}/src/locale.c"
            "${LIBC_USER_DIR}/src/asprintf.c"
    COMPILE_OPTIONS -Os ${MICROOS_FILE_WARNINGS}
)

# --- bc and dc: the calculators of arbitrary precision ------------------------
#
# Gavin D. Howard's bc 7.2.0 (BSD-2-Clause) is one library and two programs:
# `main.c` reads the name it was entered with and runs `bc_main` or `dc_main`,
# so the two images carry the same engine and the smoke tests reach the two
# names of /bin. The release has no configuration file: `Makefile.in` puts the
# settings on the command line, and `port/bc_port.h` is force-included with
# the same settings so that the release's sources are untouched.
#
# The library is built without -mgeneral-regs-only because it links the whole
# of `src/stdlib.c` for `strtol` and `strtoul`. That file also holds `strtod`
# and `qsort`, which a `double` in the text makes an object of SSE; the port
# sets CR4.OSFXSR and saves the state of the units at every switch
# (`docs/debts.md`, K9), so the choice is the one Lua made and bc may compute
# while another program does. bc itself computes in the integers of its limbs.
set(BC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/bc")
set(BC_SOURCES
    "${BC_DIR}/src/args.c"
    "${BC_DIR}/src/bc.c"
    "${BC_DIR}/src/bc_lex.c"
    "${BC_DIR}/src/bc_parse.c"
    "${BC_DIR}/src/data.c"
    "${BC_DIR}/src/dc.c"
    "${BC_DIR}/src/dc_lex.c"
    "${BC_DIR}/src/dc_parse.c"
    "${BC_DIR}/src/file.c"
    "${BC_DIR}/src/lang.c"
    "${BC_DIR}/src/lex.c"
    "${BC_DIR}/src/main.c"
    "${BC_DIR}/src/num.c"
    "${BC_DIR}/src/opt.c"
    "${BC_DIR}/src/parse.c"
    "${BC_DIR}/src/program.c"
    "${BC_DIR}/src/rand.c"
    "${BC_DIR}/src/read.c"
    "${BC_DIR}/src/vector.c"
    "${BC_DIR}/src/vm.c"
    # The help and the math library (`-l`), generated from the texts of
    # `gen/` by the release's own `gen/strgen.c`; the port note has the
    # command and the reason they are files of the tree.
    "${BC_DIR}/port/generated/bc_help.c"
    "${BC_DIR}/port/generated/dc_help.c"
    "${BC_DIR}/port/generated/lib.c"
    "${BC_DIR}/port/generated/lib2.c"
    # The two calls of the machine that the release takes from POSIX: a stat
    # of a descriptor and a signal handler.
    "${BC_DIR}/port/bc_os.c"
)

microos_userland_library(microos_user_bc FLOAT
    SOURCES ${BC_SOURCES}
    INCLUDES "${BC_DIR}/include" "${BC_DIR}/port"
    # The warnings are the ones a release of upstream C answers in the build
    # of every distribution; the exception stays here rather than in the
    # vendored source.
    COMPILE_OPTIONS "-include" "${BC_DIR}/port/bc_port.h"
        -Wno-sign-compare -Wno-unused-parameter -Wno-clobbered -Os
)

set_source_files_properties(${BC_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${BC_DIR}/port/bc_port.h")

# The programs: the same archive, the same `main`, and the name of the image
# that `argv[0]` carries. The arena is the bss of the image; the math library
# of `-l` and the bytecode of a script live in it, and 768 KiB is the room the
# format leaves (the bss is capped at 1 MiB).
foreach(calculator bc dc)
    add_libc_program(${calculator}
        FLOAT
        LIBRARIES microos_user_bc
        MAIN "${BC_DIR}/src/main.c"
        INCLUDES "${BC_DIR}/include" "${BC_DIR}/port"
        DEFINES ULIBC_HEAP_SIZE=786432u
        SOURCES "${LIBC_USER_DIR}/src/malloc.c"
                "${LIBC_USER_DIR}/src/stdlib.c"
                "${LIBC_USER_DIR}/src/setjmp.c"
                "${LIBC_USER_DIR}/src/ctype.c"
                "${LIBC_USER_DIR}/src/time.c"
                "${LIBC_USER_DIR}/src/strdup.c"
                "${LIBC_USER_DIR}/src/rand.c"
        COMPILE_OPTIONS -include "${BC_DIR}/port/bc_port.h" -Os
    )
endforeach()

# --- SQLite: the first port of a database library ---------------------------
#
# SQLite is in the public domain (third_party/sqlite/LICENSE) and its
# amalgamation is the whole library in one source. The operating system is
# this port: SQLITE_OS_OTHER=1 leaves SQLite without its `unix` layer and the
# VFS of `port/sqlite_vfs.c` answers instead, on the calls of
# components/libc/user. The library is a static archive of the guest, and the
# shell of the release is /bin/sqlite3.
#
# FLOAT is the difference: SQLite keeps a REAL in a double, and the port
# turns the XMM and x87 units on for a program that asks (ports/port_x86_64/
# CMakeLists.txt). The other programs of /bin still never touch them.
#
# The flags are the ones of a small database on a machine of one task and no
# memory management: no threads, no dynamic loading, no write-ahead log (it
# needs shared memory and mmap), no deprecated interfaces and no double-quoted
# string literals. The temporary store and the page cache live in the arena of
# the program; the journal has a limit of one megabyte, which is the whole
# size of a small database and bounds the space a transaction may ask.
set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite")
set(SQLITE_DEFINES
    SQLITE_OS_OTHER=1
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_WAL=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_DQS=0
    SQLITE_TEMP_STORE=3
    SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576
    SQLITE_DEFAULT_CACHE_SIZE=-256
    SQLITE_DEFAULT_PAGE_SIZE=4096
)

microos_userland_library(microos_user_sqlite FLOAT
    SOURCES "${SQLITE_DIR}/sqlite3.c" "${SQLITE_DIR}/port/sqlite_vfs.c"
    INCLUDES "${SQLITE_DIR}" "${SQLITE_DIR}/port"
    DEFINES ${SQLITE_DEFINES}
    # Upstream C with the warnings of this tree. The exceptions are the ones
    # of a code base that is compiled by every compiler of the world, and
    # they stay here rather than in a hunk of the amalgamation.
    COMPILE_OPTIONS -Os -Wno-sign-compare -Wno-unused-parameter
                    -Wno-missing-field-initializers -Wno-implicit-fallthrough
)

# The library of a program that is not the runtime, and the pieces this shell
# asks of it alone: the arena, the streams with fprintf, the directory, the
# information of an object, the clock, the regular expressions, the formatter
# of a double, strtod and the calls of a Unix-like system that compile even
# when they answer ENOSYS. The linker takes the objects the program names.
set(SQLITE_LIBC_SOURCES
    "${LIBC_USER_DIR}/src/malloc.c"
    "${LIBC_USER_DIR}/src/file.c"
    "${LIBC_USER_DIR}/src/stdio_ext.c"
    "${LIBC_USER_DIR}/src/ctype.c"
    "${LIBC_USER_DIR}/src/string_ext.c"
    "${LIBC_USER_DIR}/src/strdup.c"
    "${LIBC_USER_DIR}/src/stdlib.c"
    "${LIBC_USER_DIR}/src/dirent.c"
    "${LIBC_USER_DIR}/src/stat.c"
    "${LIBC_USER_DIR}/src/unistd_ext.c"
    "${LIBC_USER_DIR}/src/pwd.c"
    "${LIBC_USER_DIR}/src/time.c"
    "${LIBC_USER_DIR}/src/libgen.c"
    "${LIBC_USER_DIR}/src/termios.c"
    "${LIBC_USER_DIR}/src/regex.c"
    "${LIBC_USER_DIR}/src/getopt.c"
    "${LIBC_USER_DIR}/src/fnmatch.c"
    "${LIBC_USER_DIR}/src/assert.c"
    "${LIBC_USER_DIR}/src/format_float.c"
    "${LIBC_USER_DIR}/src/math.c"
    "${LIBC_USER_DIR}/src/sscanf.c"
    "${LIBC_USER_DIR}/src/unsupported.c"
    # The environment the shell grows (setenv/unsetenv), the canonical name
    # of a path (its `.archive` and realpath()), the reverse of strftime and
    # the names of a pattern: the files of the POSIX edge of the library that
    # the shell of the release may reach. The linker takes the objects the
    # program names, and these four are small.
    "${LIBC_USER_DIR}/src/env.c"
    "${LIBC_USER_DIR}/src/realpath.c"
    "${LIBC_USER_DIR}/src/strptime.c"
    "${LIBC_USER_DIR}/src/glob.c"
    # The command of a shell (1.8): `popen`/`pclose` are the pipe, the fork
    # and the `execve` of the child of `src/popen.c`, the words of the line
    # are `src/words.c`, and `system` is the same without a stream. The shell
    # of the release has a `.system` that reaches it.
    "${LIBC_USER_DIR}/src/popen.c"
    "${LIBC_USER_DIR}/src/system.c"
    "${LIBC_USER_DIR}/src/words.c"
    "${LIBC_USER_DIR}/src/gettimeofday.c"
    "${LIBC_USER_DIR}/src/resource.c"
    "${LIBC_USER_DIR}/src/usleep.c"
)

# The shell of the release is the program: shell.c has readline only when the
# header is there (this machine has none, so the line is a plain fgets over
# the memory stream the program prepares), and `.system`/`.edit` are left out
# with SQLITE_NOHAVE_SYSTEM - there is no shell to start from the shell of a
# database on this machine, and execve is synchronous anyway.
add_libc_program(sqlite3
    FLOAT
    MAIN "${SQLITE_DIR}/port/sqlite_shell.c"
    SOURCES ${SQLITE_LIBC_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/programs/sqlite3.c"
    LIBRARIES microos_user_sqlite
    INCLUDES "${SQLITE_DIR}" "${SQLITE_DIR}/port"
    # The arena is the bss of the program and the format caps the bss at a
    # megabyte: SQLite's page cache is 256 KiB and the script of stdin up to
    # 256 KiB, so 768 KiB leafs room for the shell and the SQL. The image of
    # the library is the largest of the port (about 800 KiB) and the ceiling
    # of the machine is 8 MiB (K_EXEC_MAX_IMAGE).
    DEFINES ULIBC_HEAP_SIZE=786432u ${SQLITE_DEFINES} SQLITE_NOHAVE_SYSTEM=1
    COMPILE_OPTIONS -Os -DULIBC_WITH_FLOAT
                    -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
                    -Wno-implicit-fallthrough
)

# --- awk: the POSIX tool of a Unix-like system -------------------------------
#
# The One True Awk is the first port that is not a library and a small main
# of the release: it is an interpreter with its own lexer, parser (a grammar
# of yacc, generated by byacc and vendored as `awkgram.tab.c`), regular
# expression engine and evaluator, and it computes in a double. `main.c` of
# the release is the program, as `lua.c` is for Lua.
#
# The port layer is `port/awk_port.h` (force-included: the C locale, the
# functions of the C library this system does not have) and `port/awk_os.c`
# (their bodies, and the refusal of a shell). Nothing of the release is
# edited. The host test compiles the same sources with the headers of the
# host, so the two builds of the tree are compared by the tests of the two.
set(AWK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/awk")
set(AWK_SOURCES
    "${AWK_DIR}/awkgram.tab.c"
    "${AWK_DIR}/b.c"
    "${AWK_DIR}/lex.c"
    "${AWK_DIR}/lib.c"
    "${AWK_DIR}/main.c"
    "${AWK_DIR}/parse.c"
    "${AWK_DIR}/proctab.c"
    "${AWK_DIR}/run.c"
    "${AWK_DIR}/tran.c"
    "${AWK_DIR}/port/awk_os.c"
)

microos_userland_library(microos_user_awk FLOAT
    SOURCES ${AWK_SOURCES}
    INCLUDES "${AWK_DIR}" "${AWK_DIR}/port"
    # The warnings are the ones upstream C answers in every build of the
    # release: a parameter a function of the table of the interpreter never
    # reads, and the `unused` of `nawk_convert` that silences a warning of
    # another compiler. They stay in the call and not in the vendored source.
    COMPILE_OPTIONS "-include" "${AWK_DIR}/port/awk_port.h"
        -Wno-unused-parameter -Wno-unused-but-set-variable -Os
)

set_source_files_properties(${AWK_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${AWK_DIR}/port/awk_port.h")

add_libc_program(awk
    FLOAT
    LIBRARIES microos_user_awk
    MAIN "${AWK_DIR}/main.c"
    INCLUDES "${AWK_DIR}" "${AWK_DIR}/port"
    # The arena is the bss of the image and the format caps the bss at a
    # megabyte. awk's symbol table, its fields and its strings live in it;
    # 512 KiB holds a script of the machine with room to spare, and the
    # image stays at 171 KiB.
    DEFINES ULIBC_HEAP_SIZE=524288u
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"
            "${LIBC_USER_DIR}/src/file.c"
            "${LIBC_USER_DIR}/src/stdio_ext.c"
            "${LIBC_USER_DIR}/src/stdlib.c"
            "${LIBC_USER_DIR}/src/string_ext.c"
            "${LIBC_USER_DIR}/src/strdup.c"
            "${LIBC_USER_DIR}/src/math.c"
            "${LIBC_USER_DIR}/src/setjmp.c"
            "${LIBC_USER_DIR}/src/time.c"
            "${LIBC_USER_DIR}/src/locale.c"
            "${LIBC_USER_DIR}/src/ctype.c"
            "${LIBC_USER_DIR}/src/assert.c"
            "${LIBC_USER_DIR}/src/format_float.c"
            "${LIBC_USER_DIR}/src/unsupported.c"
            # The command of a shell (1.8): awk's `"cmd" | getline` is
            # `popen` and its `close()` is `pclose`, and the two are the
            # pipe, the fork and the `execve` of `src/popen.c` over the
            # words of `src/words.c`. `src/unistd_ext.c` carries the calls
            # they build on (fork, pipe, dup2, execvp, waitpid) and `stat`
            # is what its `access()` asks of `src/stat.c`. `system` is not
            # here: awk answers it in its own port layer with a sentence.
            "${LIBC_USER_DIR}/src/unistd_ext.c"
            "${LIBC_USER_DIR}/src/stat.c"
            "${LIBC_USER_DIR}/src/popen.c"
            "${LIBC_USER_DIR}/src/words.c"
    COMPILE_OPTIONS -DULIBC_WITH_FLOAT -include "${AWK_DIR}/port/awk_port.h"
        -Wno-unused-parameter -Wno-unused-but-set-variable -Os
)

add_executable(microos64

    ${KERNEL_SOURCES}
    ${KSHELL_SOURCES}
    ${LIBC_SOURCES}
    ${MFS_SOURCES}
    ${FAT_SOURCES}
    ${EXT2_SOURCES}
    ${VIRTUAL_SOURCES}
    ${SETTINGS_SOURCES}
    ${HASH_SOURCES}
    ${NET_SOURCES}
    ${PKG_SOURCES}
    ${TELNET_SOURCES}
    src/boot64.S
    src/kmain.c
    src/port_x86_64.c
    src/console.c
    src/vga.c
    src/vbe.c
    src/gop.c
    src/fb.c
    src/font_spleen.c
    src/pci.c
    src/ps2.c
    src/acpi.c
    src/xhci.c
    src/usb_storage.c
    src/usb_net.c
    src/netdev.c
    src/fbdev.c
    src/inputdev.c
    src/gdt.c
    src/syscall.c
    src/paging.c
    src/mm.c
    src/cpu.c
    src/smp.c
    src/lapic.c
    src/ipi.c
    src/ioapic.c
    src/port_exec.c
    src/port_x86_64_task.c
    src/stack.c
    src/blockdev_ram.c
    src/blockdev_module.c
    src/ata.c
    src/ahci.c
    src/nvme.c
    src/virtio.c
    src/virtio_blk.c
    src/virtio_net.c
    src/e1000e.c
    src/net.c
    src/libc_shim.c
    src/tls_platform.c
    src/interrupts.S
    src/switch.S
)

target_include_directories(microos64 PRIVATE
    "${KERNEL_DIR}/include"
    "${KSHELL_DIR}/include"
    "${LIBC_DIR}/include"
    "${MFS_DIR}/include"
    "${FAT_DIR}/include"
    "${EXT2_DIR}/include"
    "${PROCFS_DIR}/include"
    "${DEVFS_DIR}/include"
    "${CONFIG_DIR}/include"
    "${TELNET_DIR}/include"
    "${SETTINGS_DIR}/include"
    "${NET_DIR}/include"
    # The hash of /etc/shadow and of a signed image (components/config,
    # components/pkg): one SHA-256 for the machine.
    "${CMAKE_CURRENT_SOURCE_DIR}/loader"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MICRO_OS_X86_NET)
    target_include_directories(microos64 PRIVATE
        "${PKG_DIR}/include"
        "${CMAKE_BINARY_DIR}"
    )
    # The public key of the build is the key of the repository when the
    # volume has none: the header is generated with the image, and the
    # compiler has to wait for it.
    target_sources(microos64 PRIVATE "${CMAKE_BINARY_DIR}/sign_public_key.h")
endif()

target_compile_definitions(microos64 PRIVATE
    KERNEL_BOARD_NAME="x86_64"
    KERNEL_BOARD_DESC="x86-64 machine"
    KERNEL_PSRAM=$<BOOL:${KERNEL_PSRAM}>
    KERNEL_EXEC=$<BOOL:${KERNEL_EXEC}>
    KERNEL_SMP=$<BOOL:${KERNEL_SMP}>
    # The ring of a pipe (kernel/kfile.h): 64 KiB, the block of one call of
    # the system, and the memory comes from kmm_alloc() with the pipe.
    KFILE_PIPE_SIZE=65536
    MICRO_OS_NET=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_HTTP=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_HTTP_SERVER=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_NTP=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_NET_MQTT=0
    MICRO_OS_TLS=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_TELNET=$<BOOL:${MICRO_OS_TELNET}>
    MICRO_OS_WIFI=$<BOOL:${MICRO_OS_WIFI}>
    MICRO_OS_PKG=$<BOOL:${MICRO_OS_X86_NET}>
    MICRO_OS_PKG_BUILTIN_KEY=$<BOOL:${MICRO_OS_X86_NET}>
    # The update of the network takes its key from /etc/ota: the key of this
    # build is a development one and is not the key of the machine.
    MICRO_OS_OTA_BUILTIN_KEY=0
    # The stack of the port lends four TCP sessions (ports/port_x86_64/src/
    # net.c, NET_TCP_SOCKETS): the server of files leaves one of them for
    # the session that is being closed and serves the other three.
    MICRO_OS_HTTPD_CONNECTIONS=3
)

if(MICRO_OS_X86_NET)
    # The bytes of mbedTLS are objects of the image, and its include path and
    # MBEDTLS_CONFIG_FILE travel with the target (third_party/mbedtls).
    target_link_libraries(microos64 PRIVATE microos_mbedtls)
endif()

if(MICRO_OS_FS_FAT32)
    target_compile_definitions(microos64 PRIVATE MICRO_OS_FS_FAT32=1)
endif()

if(MICRO_OS_FS_EXT2)
    target_compile_definitions(microos64 PRIVATE MICRO_OS_FS_EXT2=1)
endif()

# The top 2 GiB, the one region the small model of the compiler can name with
# a sign-extended 32-bit immediate. The assembly does not need it (its
# addresses are 64-bit numbers of their own) and a program has no use for it
# (it lives at 32 MiB), so the flag is for the C of the kernel only.
target_compile_options(microos64 PRIVATE $<$<COMPILE_LANGUAGE:C>:-mcmodel=kernel>
    $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>)

set_target_properties(microos64 PROPERTIES
    LINK_FLAGS "-nostdlib -static -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker64.ld -Wl,--build-id=none"
)

# --- the signature of the image (ADR 0020) ---------------------------------
#
# The kernel is a file the loader reads, and the loader verifies it before it
# copies a byte: an image with no trailer, with another digest or with a
# signature of another key never reaches the kernel. The public key is part
# of the loader, so the image cannot bring a key of its own.
#
# The key of a build is a private one chosen by MICROOS_SIGN_KEY, or a fresh
# one in the build directory when the variable is empty. The second case is
# the development loop and CI: the boot check is real, and the key is not a
# secret because it signs nothing that leaves the machine. A release sets the
# variable to the key of the release, which lives outside this tree.
set(SIGN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/loader")
set(SIGN_SHARED
    "${SIGN_DIR}/sha256.c"
    "${SIGN_DIR}/ed25519.c"
    "${SIGN_DIR}/sign_image.c"
)

add_executable(microos_sign
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/microos_sign.c"
    ${SIGN_SHARED}
)

target_include_directories(microos_sign PRIVATE "${SIGN_DIR}")

# The tool is a host program, not a kernel: the project gives every C file of
# this build -fno-pic -fno-pie, and an executable of the host has to be
# allowed to be a non-PIE one.
target_link_options(microos_sign PRIVATE -no-pie)

set(MICROOS_SIGN_KEY "" CACHE FILEPATH
    "the private key of the image (default: a key generated in the build directory)")

if(MICROOS_SIGN_KEY)
    set(SIGN_KEY "${MICROOS_SIGN_KEY}")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/signing.pub" "${CMAKE_BINARY_DIR}/sign_public_key.h"
        COMMAND $<TARGET_FILE:microos_sign> pubkey
                --key "${SIGN_KEY}"
                --pub "${CMAKE_BINARY_DIR}/signing.pub"
                --header "${CMAKE_BINARY_DIR}/sign_public_key.h"
        DEPENDS microos_sign "${SIGN_KEY}"
        COMMENT "reading the public half of the signing key"
        VERBATIM
    )
else()
    set(SIGN_KEY "${CMAKE_BINARY_DIR}/signing.key")
    # The key of a development build is generated once and kept: a rebuild
    # of the loader must not change the key of an image that is already on
    # the disk of QEMU, and a guard keeps the file across a reconfigure.
    add_custom_command(
        OUTPUT "${SIGN_KEY}" "${CMAKE_BINARY_DIR}/signing.pub"
               "${CMAKE_BINARY_DIR}/sign_public_key.h"
        COMMAND sh -c "test -f '${SIGN_KEY}' || '$<TARGET_FILE:microos_sign>' keygen --key '${SIGN_KEY}' --pub '${CMAKE_BINARY_DIR}/signing.pub' --header '${CMAKE_BINARY_DIR}/sign_public_key.h'"
        COMMAND $<TARGET_FILE:microos_sign> pubkey
                --key "${SIGN_KEY}"
                --pub "${CMAKE_BINARY_DIR}/signing.pub"
                --header "${CMAKE_BINARY_DIR}/sign_public_key.h"
        DEPENDS microos_sign
        COMMENT "generating the key of the image of this build"
        VERBATIM
    )
endif()

# The C of the loader is 32-bit and freestanding: no libc, no red zone, no
# SSE (nothing here saves an XMM register), and the oldest model of the
# processor, because the machine of QEMU is not the machine of the builder.
set(LOADER_CFLAGS
    -m32 -O2 -ffreestanding -fno-builtin -fno-stack-protector
    -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
    -mno-sse -mno-mmx -march=i386
    -Wall -Wextra -Werror -Wno-unused-parameter
    -DMICRO_OS_REQUIRE_SIGNATURE=${MICRO_OS_REQUIRE_SIGNATURE_VALUE}
    -I "${SIGN_DIR}" -I "${CMAKE_BINARY_DIR}"
)

set(LOADER_C_OBJECTS)
foreach(name sha256 ed25519 sign_image loader_io verify memory)
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/loader_${name}.o"
        COMMAND ${CMAKE_C_COMPILER} ${LOADER_CFLAGS}
                -c "${SIGN_DIR}/${name}.c"
                -o "${CMAKE_BINARY_DIR}/loader_${name}.o"
        DEPENDS "${SIGN_DIR}/${name}.c"
                "${SIGN_DIR}/sha256.h" "${SIGN_DIR}/ed25519.h"
                "${SIGN_DIR}/sign_image.h" "${SIGN_DIR}/loader_io.h"
                "${CMAKE_BINARY_DIR}/sign_public_key.h"
        COMMENT "compiling the loader's ${name}"
        VERBATIM
    )
    list(APPEND LOADER_C_OBJECTS "${CMAKE_BINARY_DIR}/loader_${name}.o")
endforeach()

# The loader is 32-bit and has no libc in it: the assembler, the compiler and
# the linker of the host do the whole job. The object of the C is 32-bit as
# well, so ld sees one machine.
# The trampoline of the application processors (ADR 0012): a flat binary
# linked at 0x7000, the page the start-up IPI names. The loader embeds it with
# .incbin and copies it there before the kernel runs, so the AP that wakes in
# real mode has a way into long mode and into C.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/ap_trampoline.o"
    COMMAND as --32 -o "${CMAKE_BINARY_DIR}/ap_trampoline.o"
            "${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.S"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.S"
    COMMENT "assembling the AP trampoline"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/ap_trampoline.bin"
    COMMAND ld -m elf_i386 -T "${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.ld"
            -o "${CMAKE_BINARY_DIR}/ap_trampoline.elf" "${CMAKE_BINARY_DIR}/ap_trampoline.o"
    COMMAND objcopy -O binary "${CMAKE_BINARY_DIR}/ap_trampoline.elf"
            "${CMAKE_BINARY_DIR}/ap_trampoline.bin"
    DEPENDS "${CMAKE_BINARY_DIR}/ap_trampoline.o"
            "${CMAKE_CURRENT_SOURCE_DIR}/ap_trampoline.ld"
    COMMENT "linking the AP trampoline at 0x7000"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/loader32.o"
    COMMAND as --32 -o "${CMAKE_BINARY_DIR}/loader32.o" "${CMAKE_CURRENT_SOURCE_DIR}/loader32.S"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loader32.S" "${CMAKE_BINARY_DIR}/ap_trampoline.bin"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "assembling the loader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/loader32.elf"
    COMMAND ld -m elf_i386 -T "${CMAKE_CURRENT_SOURCE_DIR}/loader32.ld"
            -o "${CMAKE_BINARY_DIR}/loader32.elf" "${CMAKE_BINARY_DIR}/loader32.o"
            ${LOADER_C_OBJECTS}
    DEPENDS "${CMAKE_BINARY_DIR}/loader32.o" ${LOADER_C_OBJECTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/loader32.ld"
    COMMENT "linking the loader"
    VERBATIM
)

# The flat image of the kernel and its trailer. The loader checks the trailer
# before it copies a byte, so the file QEMU gets is the signed one and the raw
# bytes are only a step of the build.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/microos64.raw"
    COMMAND objcopy -O binary "$<TARGET_FILE:microos64>" "${CMAKE_BINARY_DIR}/microos64.raw"
    DEPENDS microos64
    COMMENT "the flat image of the kernel"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/microos64.bin"
    COMMAND $<TARGET_FILE:microos_sign> sign
            --key "${SIGN_KEY}"
            --in "${CMAKE_BINARY_DIR}/microos64.raw"
            --out "${CMAKE_BINARY_DIR}/microos64.bin"
            --sig "${CMAKE_BINARY_DIR}/microos64.sig"
    DEPENDS microos_sign "${SIGN_KEY}" "${CMAKE_BINARY_DIR}/microos64.raw"
    COMMENT "signing the flat image of the kernel"
    VERBATIM
)

# --- the loader of a UEFI machine -------------------------------------------
#
# The firmware of a laptop reads one format: a PE/COFF application, the
# subsystem 10 of the specification, entered with the convention of the x86-64
# Windows ABI. The build writes it with the compiler of the host, because that
# compiler knows the target: clang has the triple of the Windows machine and
# lld-link lays the sections and the header out (loader/uefi). GNU ld has an
# emulation for PE as well and writes a header a firmware does not read, which
# is why the two tools are asked for by name and the target says what is
# missing instead of building an image that does not boot.
#
# The loader links nothing: no libc, no header of a system, and the twenty
# kilobytes of its own runtime (loader/uefi/uefi_loader.c).
find_program(MICROOS_UEFI_CC NAMES clang)
find_program(MICROOS_UEFI_LD NAMES lld-link)

set(UEFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/loader/uefi")

set(UEFI_CFLAGS
    -target x86_64-pc-windows-msvc
    -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone
    -mno-sse -mno-mmx -mno-80387
    -Wall -Wextra -Werror
    -I "${UEFI_DIR}" -I "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set(UEFI_OBJECTS "${CMAKE_BINARY_DIR}/uefi_loader.obj" "${CMAKE_BINARY_DIR}/uefi_enter.obj")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/uefi_loader.obj"
    COMMAND ${MICROOS_UEFI_CC} ${UEFI_CFLAGS} -c "${UEFI_DIR}/uefi_loader.c"
            -o "${CMAKE_BINARY_DIR}/uefi_loader.obj"
    DEPENDS "${UEFI_DIR}/uefi_loader.c" "${UEFI_DIR}/efi.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/port_x86_64_boot.h"
    COMMENT "compiling the loader of a UEFI machine"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/uefi_enter.obj"
    COMMAND ${MICROOS_UEFI_CC} -target x86_64-pc-windows-msvc -mno-red-zone -c
            "${UEFI_DIR}/uefi_enter.S" -o "${CMAKE_BINARY_DIR}/uefi_enter.obj"
    DEPENDS "${UEFI_DIR}/uefi_enter.S"
    COMMENT "assembling the jump of the UEFI loader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/BOOTX64.EFI"
    COMMAND ${MICROOS_UEFI_LD}
            /subsystem:efi_application /entry:efi_main /nodefaultlib
            /machine:x64 /base:0x10000000 /debug:none
            /out:${CMAKE_BINARY_DIR}/BOOTX64.EFI ${UEFI_OBJECTS}
    DEPENDS ${UEFI_OBJECTS}
    COMMENT "linking BOOTX64.EFI"
    VERBATIM
)

# The two loaders of the port: the multiboot image of the BIOS path and the
# application of a firmware. `make uefi` builds the second and the ESP that
# carries it; `make x86-build` the first.
if(MICROOS_UEFI_CC AND MICROOS_UEFI_LD)
    add_custom_target(uefi DEPENDS "${CMAKE_BINARY_DIR}/BOOTX64.EFI"
        COMMENT "the loader of a UEFI machine")
else()
    add_custom_target(uefi
        COMMAND ${CMAKE_COMMAND} -E echo
                "uefi: clang and lld-link are needed for BOOTX64.EFI (Arch: pacman -S clang lld)"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "the loader of a UEFI machine")
endif()

add_custom_target(image
    DEPENDS "${CMAKE_BINARY_DIR}/microos64.bin" "${CMAKE_BINARY_DIR}/loader32.elf"
            "${MAGIC_MGC}" "${MAGIC_TEXT}" ${MICROOS_PROGRAM_IMAGES}
    COMMENT "the image of the port: loader32.elf + microos64.bin + the programs of /bin"
)
