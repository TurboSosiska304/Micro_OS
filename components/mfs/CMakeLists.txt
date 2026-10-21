# Micro OS — MicroFS.

set(MICRO_OS_MFS_SRCS
    "src/mfs.c"
    "src/mfs_ram_device.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_MFS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
else()
    add_library(microos_mfs STATIC ${MICRO_OS_MFS_SRCS})
    target_include_directories(microos_mfs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_mfs PUBLIC microos_kernel)
    target_compile_features(microos_mfs PUBLIC c_std_11)
endif()
