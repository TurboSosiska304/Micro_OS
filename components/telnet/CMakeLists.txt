# Micro OS — the shell over the air: a telnet server for the local network.
#
# The component is ESP32 only: the server speaks lwIP sockets. The
# parser of the protocol (telnet_proto.c) has no sockets and no
# hardware in it and is compiled by the host tests directly.

idf_component_register(
    SRCS "src/telnet_proto.c" "src/telnet_server.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel kshell lwip
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
