# Micro OS — the shell component.
#
# The shell is portable: it only uses the console, the system information
# of the kernel and the description of the board that the kernel keeps.
#
# The check of a FAT volume is a command of the shell, so the shell asks the
# filesystem for it when the image links the reader (ADR 0009). When it does
# not, the word `fat` of `fsck` still reaches the command and is answered
# with the usage line: the shell has one more argument, not one more parser
# of volumes.

set(MICRO_OS_KSHELL_SRCS
    "src/kshell.c"
    "src/kshell_files.c"
    "src/kshell_memtest.c"
    "src/kshell_mfetch.c"
    "src/kshell_pipe.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_KSHELL_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel libc mfs fs_fat32
    )

    if(CONFIG_MICRO_OS_FS_FAT32)
        set(MICRO_OS_FS_SCOPE PUBLIC)
    else()
        # An interface target cannot carry a definition for its own sources.
        set(MICRO_OS_FS_SCOPE INTERFACE)
    endif()

    if(CONFIG_MICRO_OS_FS_FAT32)
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_FS_SCOPE} MICRO_OS_FS_FAT32=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} ${MICRO_OS_FS_SCOPE} MICRO_OS_FS_FAT32=0)
    endif()

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at build
    # time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_kshell STATIC ${MICRO_OS_KSHELL_SRCS})
    target_include_directories(microos_kshell PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_kshell PUBLIC microos_kernel microos_libc microos_mfs)
    target_compile_features(microos_kshell PUBLIC c_std_11)

    # The tests and the simulator mount and check a FAT volume, so the shell
    # of the host build carries the command when the image links the reader.
    # `microos_fs_fat32` exists only when the option of the top level build
    # put it there; the x86-64 project compiles the sources into its own
    # image and defines the flag itself.
    if(MICRO_OS_FS_FAT32)
        target_compile_definitions(microos_kshell PUBLIC MICRO_OS_FS_FAT32=1)
        target_include_directories(
            microos_kshell PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../fs_fat32/include")
        target_link_libraries(microos_kshell PUBLIC microos_fs_fat32)
    else()
        target_compile_definitions(microos_kshell PUBLIC MICRO_OS_FS_FAT32=0)
    endif()
endif()
