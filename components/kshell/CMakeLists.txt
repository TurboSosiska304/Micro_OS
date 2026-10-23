# Micro OS — the shell component.
#
# The shell is portable: it only uses the console, the memory information
# of the port and the static description of the board.

set(MICRO_OS_KSHELL_SRCS
    "src/kshell.c"
    "src/kshell_files.c"
    "src/kshell_memtest.c"
    "src/kshell_mfetch.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_KSHELL_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel libc board
    )

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_kshell STATIC ${MICRO_OS_KSHELL_SRCS})
    target_include_directories(microos_kshell PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_kshell PUBLIC microos_kernel microos_libc microos_board)
    target_compile_features(microos_kshell PUBLIC c_std_11)
endif()
