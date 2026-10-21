# Micro OS — the font and the console of the display.

set(MICRO_OS_GRAPHICS_SRCS "src/tft_console.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_GRAPHICS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel drivers font
    )
else()
    add_library(microos_graphics STATIC ${MICRO_OS_GRAPHICS_SRCS})
    target_include_directories(microos_graphics PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_graphics PUBLIC microos_kernel microos_font)
    target_compile_features(microos_graphics PUBLIC c_std_11)
endif()
