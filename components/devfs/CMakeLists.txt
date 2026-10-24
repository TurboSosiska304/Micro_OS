# Micro OS — the devfs.

set(MICRO_OS_DEVFS_SRCS "src/devfs.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_DEVFS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_devfs STATIC ${MICRO_OS_DEVFS_SRCS})
    target_include_directories(microos_devfs PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_devfs PUBLIC microos_kernel)
    target_compile_features(microos_devfs PUBLIC c_std_11)
endif()
