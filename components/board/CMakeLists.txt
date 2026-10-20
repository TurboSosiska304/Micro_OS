# Micro OS — board description component.

set(MICRO_OS_BOARD_SRCS "src/board.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_BOARD_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC KERNEL_BOARD_STICKC_PLUS2=1)
else()
    add_library(microos_board STATIC ${MICRO_OS_BOARD_SRCS})
    target_include_directories(microos_board PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_board PUBLIC microos_kernel)
    target_compile_definitions(microos_board PUBLIC KERNEL_BOARD_SIM=1)
    target_compile_features(microos_board PUBLIC c_std_11)
endif()
