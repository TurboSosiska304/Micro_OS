# Micro OS — HTTP and NTP.
#
# The protocol of both lives in portable files (the URL, the request, the
# packet and the calendar) and is tested on the host; the sockets of lwIP
# and the commands of the shell are built for the ESP32 only.

set(MICRO_OS_NET_SRCS "src/http.c" "src/ntp.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_NET_SRCS} "src/net_esp32.c"
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell drivers lwip
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_net STATIC ${MICRO_OS_NET_SRCS})
    target_include_directories(microos_net PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_SOURCE_DIR}/components/drivers/include")
    target_link_libraries(microos_net PUBLIC microos_kernel)
    target_compile_features(microos_net PUBLIC c_std_11)
endif()
