# Micro OS — the end of `project()` of a program built with the SDK.
#
# CMake sets the suffix of an executable from the system it was told about
# (`CMakeGenericSystem.cmake`, which `project()` runs) and it sets it after
# the toolchain file had its turn: the value of the toolchain is gone by the
# time the target is made. `CMAKE_PROJECT_INCLUDE` is the hook of the project
# command, so the toolchain names this file and the suffix is set once more,
# after the reset and before the first target.
#
# The suffix is not a decoration: `<TARGET>` of the link rule is the file of
# the image, and a link that wrote `hello` where the build expected
# `hello.mox` would be a file every rebuild writes again and no `install`
# copies.
#
# The language of a program is the language of the tree: the default of the
# compiler and not a `-std` of the SDK, because the sources of somebody else
# (a GPL program of `microos-progs`) are written in the dialect of their
# release and the flags of a build are not a style guide.
set(CMAKE_EXECUTABLE_SUFFIX ".mox")
