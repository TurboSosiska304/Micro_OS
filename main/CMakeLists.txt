# Micro OS — application component (ESP-IDF entry point).

idf_component_register(
    SRCS "src/main.c" "src/selftest.c"
    INCLUDE_DIRS "."
    REQUIRES kernel board port_esp32 drivers kshell mfs procfs devfs graphics libc telnet
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
