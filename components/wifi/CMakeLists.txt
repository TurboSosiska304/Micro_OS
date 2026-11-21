# Micro OS — the WiFi station: the portable core of the driver.
#
# The core is plain C11: it holds the state machine of the station, the
# configuration and the formatting, and it can be built for the host, where
# the unit tests cover it. The radio of the ESP32 is platform code and lives
# in components/port_esp32 (ADR 0004).
#
# MICRO_OS_WIFI is the switch of the whole component (ADR 0010). The radio
# of the port and the driver of the board follow the same switch: a build
# without the station links neither of them.

set(MICRO_OS_WIFI_SRCS
    "src/wifi.c"
)

if(ESP_PLATFORM)
    if(CONFIG_MICRO_OS_WIFI)
        idf_component_register(
            SRCS ${MICRO_OS_WIFI_SRCS}
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )

        # The discipline of the host build: the sources of the project are
        # compiled with the extra warnings, so a mistake is a message at
        # build time and not a fault on the board.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
        set(MICRO_OS_WIFI_SCOPE PUBLIC)
    else()
        idf_component_register(
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )
        # An interface target cannot carry a definition for its own sources.
        set(MICRO_OS_WIFI_SCOPE INTERFACE)
    endif()

    if(CONFIG_MICRO_OS_WIFI)
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_WIFI_SCOPE} MICRO_OS_WIFI=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_WIFI_SCOPE} MICRO_OS_WIFI=0)
    endif()
else()
    if(NOT MICRO_OS_WIFI)
        add_library(microos_wifi INTERFACE)
        target_include_directories(microos_wifi INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
        target_compile_features(microos_wifi INTERFACE c_std_11)
        return()
    endif()

    add_library(microos_wifi STATIC ${MICRO_OS_WIFI_SRCS})
    target_include_directories(microos_wifi PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_wifi PUBLIC microos_kernel)
    target_compile_features(microos_wifi PUBLIC c_std_11)
endif()
