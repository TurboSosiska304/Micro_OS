# Micro OS — the vendored mbedTLS, built as one object library.
#
# The sources are the upstream ones, unmodified (see README.md). This file
# names them and sets the one configuration of the tree: every translation
# unit includes `config/microos_config.h` instead of `mbedtls_config.h`.
#
# The library is an OBJECT library: an executable links it, and its objects
# are part of the image (there is no shared library on the machines of this
# system). The include directories and the definition of the configuration
# travel with it, so a consumer that links `microos_mbedtls` can include
# <mbedtls/ssl.h> and see the same switches the library was built with.
#
# The upstream code is not written in the style of this tree and it is not
# formatted by it: warnings are off for these translation units alone. The
# build of the kernel keeps -Wall -Wextra -Werror for its own sources.

file(GLOB MICRO_OS_MBEDTLS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/library/*.c")

add_library(microos_mbedtls OBJECT ${MICRO_OS_MBEDTLS_SOURCES})

# The directory of the configuration is PUBLIC as well: MBEDTLS_CONFIG_FILE
# names a header, and the consumer of the library (components/net/src/tls.c)
# includes the same build_info.h. The name is unique in the tree.
target_include_directories(microos_mbedtls
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/config"
)

target_compile_definitions(microos_mbedtls
    PUBLIC "MBEDTLS_CONFIG_FILE=\"microos_config.h\""
)

# The library was not written for this tree: its warnings are not the
# warnings of the kernel, and -Werror would make an upstream style a build
# failure. `-w` comes after the directory options, so it wins.
target_compile_options(microos_mbedtls PRIVATE -w)

target_compile_features(microos_mbedtls PUBLIC c_std_11)

# A static library would leave the objects out of the archive; an OBJECT
# library linked by an executable puts them in the image, which is what a
# freestanding machine needs. Consumers name it in target_link_libraries().
set_target_properties(microos_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE OFF)
