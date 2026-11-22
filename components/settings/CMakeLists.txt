# Micro OS — the settings of a machine (ADR 0021).
#
# A system of parameters is one file of /etc with `key=value` lines, and the
# component is the parser, the registry of the appliers and the two calls the
# console uses (`config get`, `config set`). It is portable: the machine
# registers the applier of its screen, and the component knows nothing of a
# panel, a VGA or a backlight.

set(MICRO_OS_SETTINGS_SRCS "src/settings.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_SETTINGS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_settings STATIC ${MICRO_OS_SETTINGS_SRCS})
    target_include_directories(microos_settings PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_settings PUBLIC microos_kernel)
    target_compile_features(microos_settings PUBLIC c_std_11)
endif()
