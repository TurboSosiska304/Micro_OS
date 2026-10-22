# Micro OS — ESP32 platform port (ESP-IDF component).

set(MICRO_OS_PORT_ESP32_SRCS "src/port_esp32.c")

idf_component_register(
    SRCS ${MICRO_OS_PORT_ESP32_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel esp_timer freertos esp_driver_uart vfs
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
