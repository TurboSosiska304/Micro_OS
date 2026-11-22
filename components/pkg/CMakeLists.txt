# Micro OS — the installer of programs (ADR 0024 §5).
#
# The component is a command of the shell for x86-64 only: the board has no
# installer (its `progs` partition belongs to the firmware, ADR 0018), and
# the sources are not compiled into the ESP-IDF project at all.
#
# The portable half has no sockets in it - the index, the digest, the
# signature, the database and the plan of an install - so the host tests
# drive it with a fake transport and a temporary volume. The network under
# it is the stream of components/net.

if(ESP_PLATFORM)

    # An interface target and no source: `pkg` is not a program of the board.
    idf_component_register(INCLUDE_DIRS "include")

else()

    if(NOT MICRO_OS_NET OR NOT MICRO_OS_NET_HTTP)
        # An image without the network has nothing to fetch a package with.
        # The tests and the ports that name the component still link it.
        add_library(microos_pkg INTERFACE)
        target_include_directories(microos_pkg INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
        return()
    endif()

    add_library(microos_pkg STATIC
        src/pkg.c
        src/pkg_io.c
        src/pkg_db.c
        src/pkg_index.c
        src/pkg_verify.c
    )

    target_include_directories(microos_pkg PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    # The signature of a package is the signature of an image (ADR 0020): the
    # two primitives are the ones of the loader, compiled a second time.
    target_include_directories(microos_pkg PRIVATE
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader"
    )
    target_link_libraries(microos_pkg PUBLIC microos_kernel microos_kshell microos_net)
    target_compile_features(microos_pkg PUBLIC c_std_11)
    target_compile_definitions(microos_pkg PUBLIC MICRO_OS_PKG=$<BOOL:${MICRO_OS_NET}>)
    # The key of the image is not part of a host build: the tests bring their
    # own key in /etc/pkg/keys. The x86-64 port compiles the header of the
    # build instead and says so.
    target_compile_definitions(microos_pkg PRIVATE MICRO_OS_PKG_BUILTIN_KEY=0)

endif()
