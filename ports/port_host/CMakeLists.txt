# Micro OS — host platform port and simulator.

add_library(microos_port_host STATIC
    src/port_host.c
)

target_include_directories(microos_port_host PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(microos_port_host PUBLIC microos_kernel)

add_executable(microos_sim
    src/sim_main.c
)

target_link_libraries(microos_sim PRIVATE microos_kernel microos_board microos_port_host)
