# Micro OS — the font of the display.

set(MICRO_OS_FONT_SRCS "src/font.c")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_FONT_SRCS}
        INCLUDE_DIRS "include"
    )

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
else()
    add_library(microos_font STATIC ${MICRO_OS_FONT_SRCS})
    target_include_directories(microos_font PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(microos_font PUBLIC c_std_11)
endif()
