# Micro OS — ramfs, a writable filesystem in the memory of the machine.
#
# The desk of 2.0 needs /tmp on a read-only root, and the board has no room
# for it: the component is not built on ESP32.

set(MICRO_OS_RAMFS_SRCS
    "src/ramfs_vfs.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
else()
    add_library(microos_ramfs STATIC ${MICRO_OS_RAMFS_SRCS})
    target_include_directories(microos_ramfs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_ramfs PUBLIC microos_kernel)
    target_compile_features(microos_ramfs PUBLIC c_std_11)
endif()
