# Micro OS — HTTP, NTP and MQTT: the clients, the clock and the page of the state.
#
# The whole component is portable: the protocol of each lives in plain C
# (the URL, the request, the packet and the calendar, the page of the state,
# the packet of MQTT) and the sockets come from the kernel (kernel/net.h),
# which lends them from the port - so the same sources build for the board and
# for the host tests.
#
# The component is a set of switches (ADR 0010): MICRO_OS_NET as a whole,
# MICRO_OS_NET_HTTP, _HTTP_SERVER, _NTP and _MQTT. A source of a feature that
# is off is not compiled at all, and net.c registers only the commands of the
# features that are there.

if(ESP_PLATFORM)

    # The resolver is the one part of the component a port may take without
    # the clients: the x86-64 stack asks it for a name and gets the cache,
    # and the board resolves through lwIP and links none of this.
    set(MICRO_OS_NET_SRCS "src/net.c" "src/dns.c")

    if(CONFIG_MICRO_OS_NET)
        if(CONFIG_MICRO_OS_NET_HTTP)
            list(APPEND MICRO_OS_NET_SRCS "src/http.c")
        endif()
        if(CONFIG_MICRO_OS_NET_HTTP_SERVER)
            list(APPEND MICRO_OS_NET_SRCS "src/net_http_server.c")
        endif()
        if(CONFIG_MICRO_OS_NET_NTP)
            list(APPEND MICRO_OS_NET_SRCS "src/ntp.c")
        endif()
        if(CONFIG_MICRO_OS_NET_MQTT)
            list(APPEND MICRO_OS_NET_SRCS "src/mqtt.c")
        endif()

        idf_component_register(
            SRCS ${MICRO_OS_NET_SRCS}
            INCLUDE_DIRS "include"
            REQUIRES kernel kshell
        )

        target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
        set(MICRO_OS_NET_SCOPE PUBLIC)
    else()
        # The header stays available (the commands of net are not
        # registered), but no object of the component is linked. The
        # requirements are the same as the full component: ESP-IDF reads
        # them in a pass where CONFIG_ is not known yet.
        idf_component_register(
            INCLUDE_DIRS "include"
            REQUIRES kernel kshell
        )
        # An interface target cannot carry a definition for its own sources.
        set(MICRO_OS_NET_SCOPE INTERFACE)
    endif()

    # The numbers for the C sources of the portable code. A consumer of this
    # component (main, for the registration) sees the same values.
    if(CONFIG_MICRO_OS_NET)
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_NET=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_NET=0)
    endif()

    foreach(feature MICRO_OS_NET_HTTP MICRO_OS_NET_HTTP_SERVER MICRO_OS_NET_NTP MICRO_OS_NET_MQTT)
        if(CONFIG_${feature})
            target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} ${feature}=1)
        else()
            target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} ${feature}=0)
        endif()
    endforeach()

else()

    if(NOT MICRO_OS_NET)
        # An empty library instead of no target: the simulator, the tests and
        # the null probe name the component, and a build without it has
        # nothing of it to link.
        add_library(microos_net INTERFACE)
        target_include_directories(microos_net INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
        target_compile_features(microos_net INTERFACE c_std_11)
        return()
    endif()

    set(MICRO_OS_NET_SRCS "src/net.c" "src/dns.c")

    if(MICRO_OS_NET_HTTP)
        list(APPEND MICRO_OS_NET_SRCS "src/http.c")
    endif()
    if(MICRO_OS_NET_HTTP_SERVER)
        list(APPEND MICRO_OS_NET_SRCS "src/net_http_server.c")
    endif()
    if(MICRO_OS_NET_NTP)
        list(APPEND MICRO_OS_NET_SRCS "src/ntp.c")
    endif()
    if(MICRO_OS_NET_MQTT)
        list(APPEND MICRO_OS_NET_SRCS "src/mqtt.c")
    endif()

    add_library(microos_net STATIC ${MICRO_OS_NET_SRCS})
    target_include_directories(microos_net PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_net PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_net PUBLIC c_std_11)

endif()
