# Micro OS — the C library of the system.
#
# The library is a thin layer over the ABI of the system calls: it is
# portable and it knows nothing about the kernel, only about the contract
# of the calls.

set(MICRO_OS_LIBC_SRCS
    "src/stdio.c"
    "src/stdlib.c"
    "src/unistd.c"
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MICRO_OS_LIBC_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES kernel
    )
else()
    add_library(microos_libc STATIC ${MICRO_OS_LIBC_SRCS})
    target_include_directories(microos_libc PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(microos_libc PUBLIC microos_kernel)
    target_compile_features(microos_libc PUBLIC c_std_11)
endif()
