# Micro OS — the configuration of /etc, persisted in the key-value store.
#
# The files of /etc are the configuration a program reads; the store of the
# port keeps the copy that survives a restart and a formatted volume. The
# component is portable: it adapts the store to the interface of the
# configuration, and the port (the flash of the chip, a file on the host)
# implements the store itself.

set(MICRO_OS_CONFIG_SRCS
    "src/config.c"
    "src/config_cmd.c"
    "src/config_users.c"
    "src/config_login.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_CONFIG_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell settings
    )

    # The hash of /etc/shadow is the one of the loader, the same code that
    # verifies a signed image: one SHA-256 in the tree and not two. The
    # sources of the loader are compiled into the port of the board
    # (components/port_esp32/CMakeLists.txt), so only the header is needed.
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../ports/port_x86_64/loader")

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_config STATIC ${MICRO_OS_CONFIG_SRCS})
    target_include_directories(microos_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    # The hash of a password of /etc/shadow (see the ESP branch above).
    target_include_directories(microos_config PRIVATE
        "${CMAKE_SOURCE_DIR}/ports/port_x86_64/loader")
    target_link_libraries(microos_config PUBLIC microos_kernel microos_kshell microos_settings)
    target_compile_features(microos_config PUBLIC c_std_11)
endif()
