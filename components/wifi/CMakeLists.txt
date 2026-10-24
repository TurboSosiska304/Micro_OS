# Micro OS — the WiFi station: the portable core and the radio of the ESP32.
#
# The core is plain C11: it holds the state machine of the station and can
# be built for the host, where the unit tests cover it. The radio of the
# ESP32 is the backend: it talks to the hardware through ESP-IDF.

set(MICRO_OS_WIFI_SRCS
    "src/wifi.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_WIFI_SRCS} "src/wifi_esp32.c"
        INCLUDE_DIRS "include"
        REQUIRES kernel esp_wifi esp_netif esp_event nvs_flash lwip esp_timer freertos
    )

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_wifi STATIC ${MICRO_OS_WIFI_SRCS})
    target_include_directories(microos_wifi PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_wifi PUBLIC microos_kernel)
    target_compile_features(microos_wifi PUBLIC c_std_11)
endif()
