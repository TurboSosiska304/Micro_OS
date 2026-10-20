# Micro OS — application component (ESP-IDF entry point).

idf_component_register(
    SRCS "src/main.c"
    INCLUDE_DIRS "."
    REQUIRES kernel board port_esp32
)
