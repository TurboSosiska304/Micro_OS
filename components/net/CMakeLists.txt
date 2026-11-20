# Micro OS — HTTP and NTP: the client, the clock and the page of the state.
#
# The whole component is portable: the protocol of each lives in plain C
# (the URL, the request, the packet and the calendar, the page of the state)
# and the sockets come from the kernel (kernel/net.h), which lends them from
# the port - so the same sources build for the board and for the host tests.

set(MICRO_OS_NET_SRCS "src/http.c" "src/ntp.c" "src/net.c" "src/net_http_server.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_NET_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_net STATIC ${MICRO_OS_NET_SRCS})
    target_include_directories(microos_net PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_net PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_net PUBLIC c_std_11)
endif()
