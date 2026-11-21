# Micro OS — FAT12/16/32, read and write.
#
# The component is off by default on the board: the flash of the M5StickC
# is formatted with MicroFS and a card is not in the enclosure yet (ADR
# 0009). The host tests compile the sources directly, so they are never
# left out of the test binary by a firmware option.

set(MICRO_OS_FAT_SRCS
    "src/fat_core.c"
    "src/fat_dir.c"
    "src/fat_vfs.c"
)

if(ESP_PLATFORM)
    if(CONFIG_MICRO_OS_FS_FAT32)
        idf_component_register(
            SRCS ${MICRO_OS_FAT_SRCS}
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )

        # The discipline of the host build: the sources of the project are
        # compiled with the extra warnings, so a mistake is a message at
        # build time and not a fault on the board.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
    else()
        # The component stays in the tree and keeps its header available,
        # but no object of the reader is linked into the firmware. It is an
        # interface library, so it has no sources to add warnings to.
        idf_component_register(
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )
    endif()
else()
    add_library(microos_fs_fat32 STATIC ${MICRO_OS_FAT_SRCS})
    target_include_directories(microos_fs_fat32 PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_fs_fat32 PUBLIC microos_kernel)
    target_compile_features(microos_fs_fat32 PUBLIC c_std_11)
endif()
