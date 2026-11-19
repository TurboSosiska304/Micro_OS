# Micro OS — the configuration of /etc, persisted in the key-value store.
#
# The files of /etc are the configuration a program reads; the store of the
# port keeps the copy that survives a restart and a formatted volume. The
# component is portable: it adapts the store to the interface of the
# configuration, and the port (the flash of the chip, a file on the host)
# implements the store itself.

set(MICRO_OS_CONFIG_SRCS "src/config.c" "src/config_cmd.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_CONFIG_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_config STATIC ${MICRO_OS_CONFIG_SRCS})
    target_include_directories(microos_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_config PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_config PUBLIC c_std_11)
endif()
