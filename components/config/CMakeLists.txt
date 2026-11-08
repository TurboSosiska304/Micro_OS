# Micro OS — the configuration of /etc, persisted in the NVS.
#
# The files of /etc are the configuration a program reads; the store (the
# NVS on the board) is the copy that survives a restart and a formatted
# volume. The portable part is built for the host tests as well, the NVS
# backend only for the ESP32.

set(MICRO_OS_CONFIG_SRCS "src/config.c" "src/config_cmd.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_CONFIG_SRCS} "src/config_nvs.c"
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell nvs_flash
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_config STATIC ${MICRO_OS_CONFIG_SRCS})
    target_include_directories(microos_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_config PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_config PUBLIC c_std_11)
endif()
