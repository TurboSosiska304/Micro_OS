# Micro OS — the init process and the scripts of /etc/init.d.
#
# The sources in this component contain no hardware code: they read the
# scripts through the VFS and run their lines through the shell, so the same
# code is built for the ESP32 and for the host tests.

set(MICRO_OS_INIT_SRCS "src/initd.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_INIT_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel kshell
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_init STATIC ${MICRO_OS_INIT_SRCS})
    target_include_directories(microos_init PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_init PUBLIC microos_kernel microos_kshell)
    target_compile_features(microos_init PUBLIC c_std_11)
endif()
