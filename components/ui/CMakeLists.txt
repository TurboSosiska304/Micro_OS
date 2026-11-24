# Micro OS — the library of an interface (ADR 0030).
#
# The model of a window, the canvas that draws one and the keyboard that a
# program reads: portable C with no door under it and no hardware in it, so
# the host build tests it and the programs of the port link it. It is not a
# component of the kernel: the kernel knows the screen as a device (ADR 0028)
# and nothing above the device.

set(MICRO_OS_UI_SRCS "src/window.c" "src/draw.c" "src/keyboard.c" "src/palette.c" "src/term.c"
    "src/clock.c" "client/src/wm_protocol.c" "client/src/wm_client.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_UI_SRCS}
        INCLUDE_DIRS "include" "client/include"
        REQUIRES kernel
    )

    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_ui STATIC ${MICRO_OS_UI_SRCS})
    target_include_directories(microos_ui PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
                                                 "${CMAKE_CURRENT_LIST_DIR}/client/include")
    target_link_libraries(microos_ui PUBLIC microos_kernel)
    target_compile_features(microos_ui PUBLIC c_std_11)
endif()
