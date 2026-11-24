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

    # The board keeps the copy of mbedTLS that ESP-IDF carries and no command
    # of its shell speaks TLS yet: the stub is the answer of net_tls_transport()
    # and nothing of third_party/mbedtls is built for this target (ADR 0018).
    if(CONFIG_MICRO_OS_NET)
        list(APPEND MICRO_OS_NET_SRCS "src/tls_stub.c" "src/tls_server_stub.c")
    endif()

    if(CONFIG_MICRO_OS_NET)
        if(CONFIG_MICRO_OS_NET_HTTP)
            list(APPEND MICRO_OS_NET_SRCS "src/http.c" "src/ota.c")
        endif()
        if(CONFIG_MICRO_OS_NET_HTTP_SERVER)
            # The page of the state and the server of files: two shapes of
            # one door. The board serves the first and builds the second:
            # the same sources, its own ceiling of the sessions.
            list(APPEND MICRO_OS_NET_SRCS "src/net_http_server.c" "src/httpd.c")
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

        # The signature of an image: the board's boot checks it with the
        # primitives and the key of ports/port_x86_64/loader (ADR 0020), and
        # the update of the network checks the same trailer with the same
        # key. The header of the key is generated into the build directory.
        target_include_directories(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../../ports/port_x86_64/loader"
            "${CMAKE_BINARY_DIR}"
        )
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MICRO_OS_OTA_BUILTIN_KEY=1)

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

    # The ceiling of the sessions of the server of files: a session is a task
    # of the board and its stack is 8 KiB of the internal RAM (port_esp32.c,
    # TASK_STACK_SIZE), so three of them are 24 KiB of the hundred the board
    # has, with the shell and the services of the boot beside them. The host
    # and x86-64 name their own.
    target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_HTTPD_CONNECTIONS=3)

    # The numbers for the C sources of the portable code. A consumer of this
    # component (main, for the registration) sees the same values.
    if(CONFIG_MICRO_OS_NET)
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_NET=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_NET=0)
    endif()

    target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_NET_SCOPE} MICRO_OS_TLS=0)

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

    # The client of DHCP is here in every build of the component: the stack of
    # the x86-64 port asks it for the address of the machine at the first use
    # of the network, and the client itself is portable (net/dhcp.h). Its test
    # is the server the case of tests/cases/test_dhcp.c scripts.
    set(MICRO_OS_NET_SRCS "src/net.c" "src/dns.c" "src/dhcp.c")

    if(MICRO_OS_NET_HTTP)
        list(APPEND MICRO_OS_NET_SRCS "src/http.c")
        # The update over the network: the download and its check are
        # portable, and the slots are the machine's (net/ota.h).
        list(APPEND MICRO_OS_NET_SRCS "src/ota.c" "src/ota_file.c")
    endif()
    if(MICRO_OS_NET_HTTP_SERVER)
        list(APPEND MICRO_OS_NET_SRCS "src/net_http_server.c" "src/httpd.c")
    endif()
    if(MICRO_OS_NET_NTP)
        list(APPEND MICRO_OS_NET_SRCS "src/ntp.c")
    endif()
    if(MICRO_OS_NET_MQTT)
        list(APPEND MICRO_OS_NET_SRCS "src/mqtt.c")
    endif()

    # The TLS client, or the stub that says the image has none. mbedTLS is
    # vendored (third_party/mbedtls) and linked as an object library, so its
    # bytes are part of the simulator and of the tests that name it.
    if(MICRO_OS_TLS)
        list(APPEND MICRO_OS_NET_SRCS "src/tls.c" "src/tls_server.c")
    else()
        list(APPEND MICRO_OS_NET_SRCS "src/tls_stub.c" "src/tls_server_stub.c")
    endif()

    add_library(microos_net STATIC ${MICRO_OS_NET_SRCS})
    target_include_directories(microos_net PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    # The hash of a download (the --sha256 of wget) is the hash of the
    # loader, the same code that checks a signed image: one SHA-256 for the
    # machine, and the port links its object (ports/*/CMakeLists.txt).
    target_include_directories(microos_net
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../ports/port_x86_64/loader")
    target_link_libraries(microos_net PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_net PUBLIC c_std_11)
    # The host and x86-64 bring the key of an update in /etc/ota: a build
    # whose key is a development one does not carry a second copy of it.
    target_compile_definitions(microos_net PRIVATE MICRO_OS_OTA_BUILTIN_KEY=0)

    if(MICRO_OS_TLS)
        target_link_libraries(microos_net PUBLIC microos_mbedtls)
        # The headers of mbedTLS test optional switches with #if without
        # defining every one of them (`MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT`
        # is undefined when DTLS is off, and read as a number later). The
        # library is built with -w for the same reason; this one file of the
        # kernel includes its headers, and -Wundef of this tree would make
        # an upstream style a build error.
        set_source_files_properties("src/tls.c" "src/tls_server.c"
            PROPERTIES COMPILE_OPTIONS "-Wno-undef")
    endif()

endif()
