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
