# Micro OS — the shell over the air: a telnet server for the local network.
#
# The component is portable: the sockets of the session come from the kernel
# (kernel/net.h), which lends them from the port, and telnet_proto.c has no
# sockets and no hardware in it at all. The host build has the server too, so
# the sanitizers and the tests of the loopback exercise the code the board
# runs.
#
# MICRO_OS_TELNET is the switch of the whole component (ADR 0010): a build
# that leaves it out links no server and registers no command.

set(MICRO_OS_TELNET_SRCS "src/telnet_proto.c" "src/telnet_server.c")

if(ESP_PLATFORM)
    if(CONFIG_MICRO_OS_TELNET)
        idf_component_register(
            SRCS ${MICRO_OS_TELNET_SRCS}
            INCLUDE_DIRS "include"
            REQUIRES kernel kshell config
        )

        # The discipline of the host build: the sources of the project are
        # compiled with the extra warnings, so a mistake is a message at
        # build time and not a fault on the board.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
        set(MICRO_OS_TELNET_SCOPE PUBLIC)
    else()
        # The requirements are the same as the full component: ESP-IDF reads
        # them in a pass where CONFIG_ is not known yet.
        idf_component_register(
            INCLUDE_DIRS "include"
            REQUIRES kernel kshell config
        )
        # An interface target cannot carry a definition for its own sources.
        set(MICRO_OS_TELNET_SCOPE INTERFACE)
    endif()

    if(CONFIG_MICRO_OS_TELNET)
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_TELNET_SCOPE} MICRO_OS_TELNET=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_TELNET_SCOPE} MICRO_OS_TELNET=0)
    endif()
else()
    if(NOT MICRO_OS_TELNET)
        add_library(microos_telnet INTERFACE)
        target_include_directories(microos_telnet INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
        target_compile_features(microos_telnet INTERFACE c_std_11)
        return()
    endif()

    add_library(microos_telnet STATIC ${MICRO_OS_TELNET_SRCS})
    target_include_directories(microos_telnet PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    # The session of a client is the one of config/session.h: the accounts of
    # /etc and the dialogue of the door belong to config, and the server of
    # telnet is the transport that hands it a socket.
    target_link_libraries(microos_telnet PUBLIC microos_kernel microos_kshell microos_config)
    target_compile_features(microos_telnet PUBLIC c_std_11)
endif()
