# Micro OS — the hello of the SDK.
#
# The whole CMake of a program that is built outside the tree of the kernel:
# a name, a source, and the toolchain of the SDK. There is no include path to
# add, no library to link and no linker flag to know: the toolchain
# (`share/microos-toolchain.cmake`) sets the headers, the crt, the library of
# a program and the link of the `MOX1` format, so `add_executable` is the
# recipe.
#
#     cmake -B build -S share/examples/hello \
#         -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/microos-toolchain.cmake
#     cmake --build build
#
# The result is `build/hello.mox` (a property of the toolchain: the suffix of
# an executable of this machine is `.mox`). A machine runs it from `/bin`;
# `tools/x86_image.py --program /bin/hello=build/hello.mox` of the kernel tree
# writes it into the disk image of QEMU.
cmake_minimum_required(VERSION 3.16)

project(microos_hello C)

add_executable(hello hello.c)

message(STATUS "the SDK of Micro OS: ${MICROOS_SDK_ROOT}")
