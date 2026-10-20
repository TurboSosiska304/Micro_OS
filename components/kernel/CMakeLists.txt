# Micro OS — portable kernel core.
#
# The sources in this component contain no platform specific code: they can
# be compiled for the ESP32 target (as an ESP-IDF component) and for the
# host (as a plain static library used by the unit tests).

set(MICRO_OS_KERNEL_SRCS
    "src/boot.c"
    "src/kmod.c"
    "src/log.c"
    "src/panic.c"
    "src/sched.c"
    "src/task.c"
    "src/version.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_KERNEL_SRCS}
        INCLUDE_DIRS "include"
    )
else()
    add_library(microos_kernel STATIC ${MICRO_OS_KERNEL_SRCS})
    target_include_directories(microos_kernel PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(microos_kernel PUBLIC c_std_11)
endif()
