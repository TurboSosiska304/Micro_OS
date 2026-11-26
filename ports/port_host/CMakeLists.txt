# Micro OS — host platform port and simulator.

add_library(microos_port_host STATIC
    src/blockdev_file.c
    src/port_host.c
    src/port_exec.c
    src/port_host_psram.c
    src/port_kv_file.c
    src/port_net_posix.c
    src/stubs/host_clock.c
    src/stubs/host_commands.c
    src/stubs/host_modules.c
    src/stubs/host_render.c
    src/stubs/host_stubs.c
    src/tls_platform.c
)

target_include_directories(microos_port_host PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
# The entropy of the host answers net/tls_platform.h of the network
# component; the protocol half of TLS is in components/net.
target_include_directories(microos_port_host PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/net/include")
target_link_libraries(microos_port_host PUBLIC microos_kernel microos_kshell)
find_package(Threads REQUIRED)
target_link_libraries(microos_port_host PUBLIC Threads::Threads)
# The host port talks to POSIX (sockets, netdb, pthread): it needs the
# extensions of the system, so it opts into gnu23 on its own target.
set_target_properties(microos_port_host PROPERTIES
    C_STANDARD 23
    C_EXTENSIONS ON
)

add_executable(microos_sim
    src/sim_main.c
    # The hash of a password of /etc/shadow and of a signed image: the same
    # SHA-256 and Ed25519 as the loader. The simulator links the whole
    # system, so it carries the primitives of the tree as the board does -
    # the update of components/net checks the trailer of an image with them.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ports/port_x86_64/loader/sha256.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ports/port_x86_64/loader/ed25519.c"
)
target_include_directories(microos_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ports/port_x86_64/loader")

# The simulator loads the whole portable system, not only the shell: the
# volume of MicroFS, /proc, /dev, the configuration of /etc, the scripts of
# /etc/init.d and the shell. The list is the one the firmware links in
# main/src/main.c, minus the ESP32 drivers: the stubs of the host take their
# place (src/stubs).
#
# The kernel and the port are named twice on purpose: the archives are
# scanned once, from left to right, and the key-value store is a kernel
# object that calls into the port - so the port has to appear again after
# the kernel object that asked for it.
target_link_libraries(microos_sim PRIVATE
    microos_kernel
    microos_board
    microos_mfs
    microos_procfs
    microos_devfs
    microos_config
    microos_init
    microos_kshell
    microos_graphics
    microos_net
    microos_port_host
    microos_kernel
    microos_kshell
    microos_port_host
)
