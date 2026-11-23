# Micro OS — ext2, read and written.
#
# The filesystem is off by default on the board and on x86-64: no image
# mounts an ext2 volume at the boot, and a reader that no call asks for is
# an object file of no use. The host build compiles and links it always, so
# the tests are the place where every path of it is exercised (ADR 0019).

set(MICRO_OS_EXT2_SRCS
    "src/ext2_core.c"
    "src/ext2_dir.c"
    "src/ext2_write.c"
    "src/ext2_check.c"
    "src/ext2_vfs.c"
)

if(ESP_PLATFORM)
    if(CONFIG_MICRO_OS_FS_EXT2)
        idf_component_register(
            SRCS ${MICRO_OS_EXT2_SRCS}
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )

        # The discipline of the host build: the sources of the project are
        # compiled with the extra warnings, so a mistake is a message at
        # build time and not a fault on the board.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
    else()
        # The component stays in the tree and keeps its header available,
        # but no object of the reader is linked into the firmware.
        idf_component_register(
            INCLUDE_DIRS "include"
            REQUIRES kernel
        )
    endif()
else()
    add_library(microos_fs_ext2 STATIC ${MICRO_OS_EXT2_SRCS})
    target_include_directories(microos_fs_ext2 PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_fs_ext2 PUBLIC microos_kernel)
    target_compile_features(microos_fs_ext2 PUBLIC c_std_11)
endif()
