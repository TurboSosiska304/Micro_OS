# Micro OS — the tools of the host build.
#
# The only tool here is the builder of the disk image of the x86-64 port: a
# host program that links the real MicroFS (components/mfs) and writes the
# programs into a volume, so that the format has one implementation and the
# tests can mount what the machine boots.

add_library(mkfs_mfs_image STATIC mkfs_mfs_image.c)

target_include_directories(mkfs_mfs_image PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/components/kernel/include"
    "${CMAKE_SOURCE_DIR}/components/mfs/include"
)

target_link_libraries(mkfs_mfs_image PUBLIC microos_mfs)

add_executable(microos_mkfs mkfs_mfs.c)

target_link_libraries(microos_mkfs PRIVATE
    mkfs_mfs_image
    microos_mfs
    microos_kernel
    microos_kshell
    microos_port_host
    # The archives are scanned once, left to right, and the kernel object
    # that asks the port for a fork is only pulled by a later scan: the port
    # is named again after it, the way the simulator does it.
    microos_kernel
    microos_kshell
    microos_port_host
)

# The load of the campaign of 1.0 on the host: several tasks, the allocator,
# pipes, the VFS and a fork child, for the race detector and for a leak. It
# is the whole portable kernel over the host port, the way the tests build
# it, and no hardware.
add_executable(microos_stress host_stress.c)

target_include_directories(microos_stress PRIVATE
    "${CMAKE_SOURCE_DIR}/components/kernel/include"
    "${CMAKE_SOURCE_DIR}/components/mfs/include"
    "${CMAKE_SOURCE_DIR}/ports/port_host/include"
)

target_link_libraries(microos_stress PRIVATE
    microos_kernel
    microos_mfs
    microos_port_host
    # The same second scan as the tool above: an archive is read once.
    microos_kernel
    microos_mfs
    microos_port_host
)
