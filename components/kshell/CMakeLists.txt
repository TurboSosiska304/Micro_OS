# Micro OS — the shell component.
#
# The shell is portable: it only uses the console and the memory
# information of the port.

set(MICRO_OS_KSHELL_SRCS
    "src/kshell.c"
    "src/kshell_files.c"
    "src/kshell_memtest.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_KSHELL_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
else()
    add_library(microos_kshell STATIC ${MICRO_OS_KSHELL_SRCS})
    target_include_directories(microos_kshell PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_kshell PUBLIC microos_kernel)
    target_compile_features(microos_kshell PUBLIC c_std_11)
endif()
