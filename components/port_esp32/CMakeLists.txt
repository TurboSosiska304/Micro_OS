# Micro OS — ESP32 platform port (ESP-IDF component).

set(MICRO_OS_PORT_ESP32_SRCS "src/port_esp32.c")

idf_component_register(
    SRCS ${MICRO_OS_PORT_ESP32_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel esp_timer
)
