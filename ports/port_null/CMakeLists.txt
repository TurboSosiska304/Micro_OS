# Micro OS — the port that is not a machine.
#
# The kernel and every portable component are compiled and linked against
# these stubs by the host build: a dependency on POSIX, on a driver or on a
# chip shows up here as a link error, on a laptop. See the header of
# src/port_null.c.

add_library(microos_port_null STATIC
    src/port_null.c
    src/port_null_task.c
)

target_include_directories(microos_port_null PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(microos_port_null PUBLIC microos_kernel)
target_compile_features(microos_port_null PUBLIC c_std_11)

# The kernel and the shell over the null port: if anything portable takes a
# symbol from a machine, this link fails.
add_executable(microos_null_probe src/probe.c)

# The port is linked whole: it is the bottom of the graph, the kernel calls
# it from every corner, and a member of a static archive that nothing has
# asked for yet is left behind by the linker. Whole-archive puts every stub
# in place before anything calls it.
target_link_libraries(microos_null_probe PRIVATE
    -Wl,--whole-archive
    microos_port_null
    -Wl,--no-whole-archive
    microos_kernel
    microos_kshell
    microos_libc
)

add_test(NAME null_probe COMMAND microos_null_probe)
