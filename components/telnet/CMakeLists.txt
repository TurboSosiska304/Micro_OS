# Micro OS — the shell over the air: a telnet server for the local network.
#
# The component is portable: the sockets of the session come from the kernel
# (kernel/net.h), which lends them from the port, and telnet_proto.c has no
# sockets and no hardware in it at all. The host build has the server too, so
# the sanitizers and the tests of the loopback exercise the code the board
# runs.

set(MICRO_OS_TELNET_SRCS "src/telnet_proto.c" "src/telnet_server.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_TELNET_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell
    )

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_telnet STATIC ${MICRO_OS_TELNET_SRCS})
    target_include_directories(microos_telnet PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_telnet PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_telnet PUBLIC c_std_11)
endif()
