# Micro OS — the procfs.

set(MICRO_OS_PROCFS_SRCS "src/procfs.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_PROCFS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
else()
    add_library(microos_procfs STATIC ${MICRO_OS_PROCFS_SRCS})
    target_include_directories(microos_procfs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_procfs PUBLIC microos_kernel)
    target_compile_features(microos_procfs PUBLIC c_std_11)
endif()
